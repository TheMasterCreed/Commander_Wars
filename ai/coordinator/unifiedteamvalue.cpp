#include "ai/coordinator/unifiedteamvalue.h"

namespace Coordinator
{
    namespace
    {
        constexpr std::size_t TEAM_HASH_MIX = 0x9e3779b97f4a7c15ULL;
        constexpr std::size_t TEAM_HASH_LEFT_SHIFT = 6;
        constexpr std::size_t TEAM_BITS_PER_WORD = 64;
        constexpr std::size_t TEAM_BIT_WORD_SHIFT = 6;
        constexpr std::size_t TEAM_BIT_WORD_MASK = TEAM_BITS_PER_WORD - 1;
        constexpr std::size_t TEAM_REACH_INITIAL_BUCKETS = 64;
        constexpr std::size_t TEAM_SEQ_CACHE_INITIAL_BUCKETS = 1024;
        constexpr std::size_t TEAM_OPEN_SITE_CACHE_BUCKETS = 64;

        inline void teamHashCombine(std::size_t & seed, std::size_t v) noexcept
        {
            seed ^= v + TEAM_HASH_MIX + (seed << TEAM_HASH_LEFT_SHIFT) + (seed >> 2);
        }

        struct ReachCacheKey
        {
            int moverKi{0};
            int effGrid{0};
            int startIdx{0};
            int moveBudget{0};
            int ignoredIdx{-1};
            friend bool operator==(const ReachCacheKey &, const ReachCacheKey &) = default;
        };

        struct ReachCacheKeyHash
        {
            std::size_t operator()(const ReachCacheKey & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(k.moverKi) * TEAM_HASH_MIX;
                teamHashCombine(h, static_cast<std::size_t>(k.effGrid));
                teamHashCombine(h, static_cast<std::size_t>(k.startIdx));
                teamHashCombine(h, static_cast<std::size_t>(k.moveBudget));
                teamHashCombine(h, static_cast<std::size_t>(k.ignoredIdx));
                return h;
            }
        };

        struct ReachBitEntry
        {
            std::size_t reachOff{0};
            std::size_t touchedOff{0};
        };

        struct OccReachCache
        {
            std::unordered_map<ReachCacheKey, ReachBitEntry, ReachCacheKeyHash> entries;
            std::vector<std::uint64_t> bitPool;

            void init()
            {
                entries.reserve(TEAM_REACH_INITIAL_BUCKETS);
            }

            void clear()
            {
                entries.clear();
                bitPool.clear();
            }
        };

        struct SeqStockCacheKey
        {
            ContinuationKey key;
            std::vector<std::uint8_t> blocked;
            friend bool operator==(const SeqStockCacheKey &, const SeqStockCacheKey &) = default;
        };

        struct SeqStockCacheKeyHash
        {
            std::size_t operator()(const SeqStockCacheKey & k) const noexcept
            {
                std::size_t h = k.key.size() * TEAM_HASH_MIX;
                CanonicalPlanActionKeyHash actHash;
                for (const auto & act : k.key)
                    teamHashCombine(h, actHash(act));
                for (const std::uint8_t b : k.blocked)
                    teamHashCombine(h, static_cast<std::size_t>(b));
                return h;
            }
        };
    } // namespace

    struct UnifiedTeamEvaluator::FastCache
    {
        std::size_t bitWords{0};
        OccReachCache friendlyReach;
        OccReachCache enemyReach;
        std::vector<const std::vector<std::int32_t> *> flatTileDist;
        std::vector<const std::vector<std::int32_t> *> flatTileAdjDist;
        std::vector<const std::vector<std::int32_t> *> flatMeleeRingDist;
        std::unordered_map<SeqStockCacheKey, MilliFunds, SeqStockCacheKeyHash> seqStockCache;
        std::unordered_map<std::uint64_t, MilliFunds> openSiteValueCache;
        std::vector<std::uint8_t> nodeNearEnemyFactory;
    };

    std::vector<PlanRowAction> buildPostReplyStockActions(
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

    int mostValuableExposedUnit(
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
        for (const auto & obs : local.observedDefenders)
        {
            if (!obs.facts || !obs.facts->tile || obs.facts->tile->shooters.empty())
                continue;
            if (obs.cost > bestCost || (obs.cost == bestCost && (bestUnit == NO_UNIT || obs.engineUnitId < bestUnit)))
            {
                bestCost = obs.cost;
                bestUnit = obs.engineUnitId;
            }
        }
        return bestUnit;
    }

    std::vector<int> allExposedUnits(
        const MasterAssignmentProblem & problem, const CombatTerrain & local, std::span<const int> choices)
    {
        std::vector<std::pair<MilliFunds, int>> ranked;
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
            ranked.emplace_back(local.units[static_cast<std::size_t>(ki)].replacementCost, member.engineUnitId);
        }
        for (const auto & obs : local.observedDefenders)
        {
            if (obs.facts && obs.facts->tile && !obs.facts->tile->shooters.empty())
                ranked.emplace_back(obs.cost, obs.engineUnitId);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto & a, const auto & b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        std::vector<int> ids;
        ids.reserve(ranked.size());
        for (const auto & [cost, unitId] : ranked)
        {
            (void)cost;
            ids.push_back(unitId);
        }
        return ids;
    }

    std::vector<int> allExposedActiveCappers(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        const CombatTerrain & local, std::span<const int> choices)
    {
        std::vector<std::pair<MilliFunds, int>> ranked;
        for (const auto & member : problem.actors())
        {
            if (member.stockRow == NO_STOCK_ROW) continue;
            const int slot = member.canonicalSlot;
            if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
            const int opt = choices[static_cast<std::size_t>(slot)];
            if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
            if (!cand.capture.has_value() || !cand.source.replyFacts || !cand.source.replyFacts->tile ||
                cand.source.replyFacts->tile->shooters.empty())
                continue;
            const int ki = member.knowledgeUnitIndex;
            const MilliFunds cost = (ki >= 0 && ki < static_cast<int>(local.units.size()))
                ? local.units[static_cast<std::size_t>(ki)].replacementCost
                : cand.source.actor.replacementCost;
            (void)knowledge;
            ranked.emplace_back(cost, member.engineUnitId);
        }
        for (const auto & obs : local.observedDefenders)
        {
            if (!obs.facts || !obs.facts->tile || obs.facts->tile->shooters.empty())
                continue;
            if (obs.knowledgeIndex >= 0 && obs.knowledgeIndex < static_cast<int>(knowledge.units().size()) &&
                knowledge.units()[static_cast<std::size_t>(obs.knowledgeIndex)].capturePoints > 0)
            {
                ranked.emplace_back(obs.cost, obs.engineUnitId);
            }
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto & a, const auto & b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        std::vector<int> ids;
        ids.reserve(ranked.size());
        for (const auto & [cost, unitId] : ranked)
        {
            (void)cost;
            ids.push_back(unitId);
        }
        return ids;
    }

    UnifiedTeamEvaluator::UnifiedTeamEvaluator(const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
                         const PropertyStockField & field, std::span<const PropertyFacts> properties,
                         CombatTerrain & local, const ReplyBoard & actingBoard, std::int64_t replayTariff,
                         std::int64_t & work, UnifiedHqRankFn hqRankFn, UnifiedBuildSlotsFn buildSlotsFn,
                         const UnifiedProductionModel * productionModel)
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
          m_productionModel(productionModel),
          m_maxDist(std::max(1, local.width + local.height)),
          m_unitCount(local.units.size()),
          m_cellCount(static_cast<std::size_t>(std::max(0, local.width)) *
                      static_cast<std::size_t>(std::max(0, local.height)))
    {
        constexpr std::size_t PATH_CACHE_INITIAL_BUCKETS = 256;
        m_tilePathDistCache.reserve(PATH_CACHE_INITIAL_BUCKETS);
        m_indirectTilePathCache.reserve(PATH_CACHE_INITIAL_BUCKETS);
        m_fast = std::make_shared<FastCache>();
        m_fast->bitWords = (m_cellCount + TEAM_BITS_PER_WORD - 1) / TEAM_BITS_PER_WORD;
        m_fast->friendlyReach.init();
        m_fast->enemyReach.init();
        const std::size_t flatSlots = local.grids.size() * m_cellCount;
        m_fast->flatTileDist.assign(flatSlots, nullptr);
        m_fast->flatTileAdjDist.assign(flatSlots, nullptr);
        m_fast->flatMeleeRingDist.assign(flatSlots, nullptr);
        m_fast->seqStockCache.reserve(TEAM_SEQ_CACHE_INITIAL_BUCKETS);
        m_fast->openSiteValueCache.reserve(TEAM_OPEN_SITE_CACHE_BUCKETS);
        m_bfsDist.assign(m_cellCount, std::numeric_limits<std::int32_t>::max());
        m_bfsGen.assign(m_cellCount, 0);
        m_occFriendlyGen.assign(m_cellCount, 0);
        m_occFriendlyKi.assign(m_cellCount, NO_UNIT);
        m_occEnemyGen.assign(m_cellCount, 0);
        m_occEnemyKi.assign(m_cellCount, NO_UNIT);
        m_unitRanges.resize(m_unitCount, {1, 1});
        for (int ki = 0; ki < static_cast<int>(m_unitCount); ++ki)
            m_unitRanges[static_cast<std::size_t>(ki)] = unitAttackRangeUncached(ki);
        for (std::size_t i = 0; i < local.units.size(); ++i)
        {
            const auto & pawn = local.units[i];
            if (pawn.present && pawn.enemy && pawn.tile != INVALID_TILE)
            {
                m_enemyPositions.push_back(pawn.tile);
                m_enemyUnits.emplace_back(static_cast<int>(i), pawn.tile);
            }
            if (m_sampleCapperKi == NO_UNIT && pawn.present && !pawn.enemy && pawn.capper)
                m_sampleCapperKi = static_cast<int>(i);
        }
        for (const auto & prop : properties)
        {
            const Relation rel = knowledge.relation(prop.ownerId);
            const bool isEnemyProp = prop.ownerId != NO_OWNER &&
                (knowledge.observerId() == NO_OWNER ? true : rel == Relation::Enemy);
            const bool unownedOrEnemy = prop.ownerId == NO_OWNER || isEnemyProp;
            if (prop.capturable && unownedOrEnemy)
            {
                m_unownedProperties.push_back({prop.x, prop.y});
                if (isEnemyProp)
                    m_enemyOwnedProperties.push_back({prop.x, prop.y});
                if (prop.ownerId == NO_OWNER)
                    m_unownedNeutralProperties.push_back({prop.x, prop.y});
                if (prop.canProduce)
                    m_unownedFactories.push_back({prop.x, prop.y});
            }
            if (prop.canProduce && isEnemyProp)
                m_enemyFactories.push_back({prop.x, prop.y});
            if (prop.canProduce && prop.ownerId == knowledge.observerId())
                m_friendlyFactories.push_back({prop.x, prop.y});
            if (prop.ownerId == knowledge.observerId())
                m_friendlyRepairSites.push_back({prop.x, prop.y});
            const bool protectCapture = prop.ownerId == knowledge.observerId() || rel == Relation::Allied;
            if (prop.capturable && protectCapture)
            {
                m_ownCapturableProperties.push_back(OwnCapturableProperty{
                    .tile = {prop.x, prop.y},
                    .income = captureIncome(prop),
                    .isHq = prop.objective == ObjectiveKind::Headquarters,
                });
            }
        }
        m_local.terrainAwareFiringTile = true;
        if (m_productionModel != nullptr && m_productionModel->valid())
        {
            m_strategicBonusTiles = m_productionModel->strategicCaptureTiles();
        }
        if (m_stockContext.valid())
        {
            for (std::size_t n = 0; n < m_stockContext.nodeCount(); ++n)
            {
                if (m_stockContext.nodeOwner(n) != OwnerSign::Neutral) continue;
                const TilePoint tile = m_stockContext.nodeTile(n);
                if (tile == INVALID_TILE) continue;
                if (!m_stockContext.enemyClaimsNeutralBeforeUs(n))
                    m_deliverableNeutralProperties.push_back(tile);
            }
        }
        else
        {
            m_deliverableNeutralProperties = m_unownedNeutralProperties;
        }
        computeBreakableProfiles();
        precomputeWeaponTables();
        precomputeForwardEnemies();
        buildPathDistances();
        m_actorHasSafeCandidate.assign(m_problem.actors().size(), 0);
        m_actorHasSafeStayCapture.assign(m_problem.actors().size(), 0);
        ContinuationKey initCapKey;
        for (const auto & member : m_problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (slot < 0 || slot >= static_cast<int>(m_actorHasSafeCandidate.size()))
                continue;
            const bool hasSafe = std::ranges::any_of(member.candidates, [](const MasterCandidate & c) {
                return c.plannable &&
                    (!c.source.replyFacts || !c.source.replyFacts->tile ||
                     c.source.replyFacts->tile->shooters.empty());
            });
            if (hasSafe)
                m_actorHasSafeCandidate[static_cast<std::size_t>(slot)] = 1;
            const int ki = member.knowledgeUnitIndex;
            const bool hasPartialCarry = ki >= 0 && ki < static_cast<int>(m_knowledge.units().size()) &&
                m_knowledge.units()[static_cast<std::size_t>(ki)].canCapture &&
                m_knowledge.units()[static_cast<std::size_t>(ki)].capturePoints > 0;
            const bool hasSafeStayCap = hasPartialCarry && std::ranges::any_of(member.candidates, [](const MasterCandidate & c) {
                return c.plannable && c.capture.has_value() && c.destination == c.origin &&
                    (!c.source.replyFacts || !c.source.replyFacts->tile ||
                     c.source.replyFacts->tile->shooters.empty());
            });
            if (hasSafeStayCap)
                m_actorHasSafeStayCapture[static_cast<std::size_t>(slot)] = 1;
            if (m_stockContext.valid() && member.stockRow != NO_STOCK_ROW &&
                member.stockRow >= 0 && static_cast<std::size_t>(member.stockRow) < m_stockContext.rowCount())
            {
                const MasterCandidate * chosenInit = nullptr;
                for (const auto & c : member.candidates)
                {
                    if (!c.plannable || !c.capture.has_value())
                        continue;
                    const bool safe = !c.source.replyFacts || !c.source.replyFacts->tile ||
                        c.source.replyFacts->tile->shooters.empty();
                    if (c.capture->turnsUntilOwned <= 0 || safe)
                    {
                        chosenInit = &c;
                        if (c.capture->turnsUntilOwned <= 0)
                            break;
                    }
                }
                if (chosenInit != nullptr)
                    initCapKey.push_back(CanonicalPlanActionKey::fromAction(
                        member.stockRow, chosenInit->destination.x, chosenInit->destination.y, true));
                else if (!member.candidates.empty())
                    initCapKey.push_back(CanonicalPlanActionKey::fromAction(
                        member.stockRow, member.candidates.front().origin.x, member.candidates.front().origin.y, false));
            }
        }
        if (m_stockContext.valid() && !initCapKey.empty())
        {
            std::sort(initCapKey.begin(), initCapKey.end());
            const auto tuned = m_stockContext.tune(initCapKey, UNIFIED_STOCK_TUNE_ROUNDS);
            m_stockTuned = true;
            if (tuned.status == ReplyStockStatus::Valid && tuned.floor.status == ReplyStockStatus::Valid)
                m_baselineFloor = tuned.floor;
            if (m_stockContext.priceVersion() == 0)
            {
                const std::vector<MilliFunds> zeroPrices(m_stockContext.nodeCount(), 0);
                m_stockContext.installPrices(zeroPrices);
            }
        }
        if (m_fast && m_stockContext.valid())
        {
            const std::size_t nodeCount = m_stockContext.nodeCount();
            m_fast->nodeNearEnemyFactory.assign(nodeCount, 0);
            for (std::size_t n = 0; n < nodeCount; ++n)
            {
                const TilePoint tile = m_stockContext.nodeTile(n);
                if (tile == INVALID_TILE)
                    continue;
                for (const TilePoint f : m_enemyFactories)
                {
                    if (enemyFactoryThreatensTile(f, tile))
                    {
                        m_fast->nodeNearEnemyFactory[n] = 1;
                        break;
                    }
                }
            }
        }
    }

    int UnifiedTeamEvaluator::pawnTravelGrid(const CombatPawn & pawn)
    {
        return pawn.travelGrid >= 0 ? pawn.travelGrid : pawn.grid;
    }

    std::span<const TilePoint> UnifiedTeamEvaluator::enemyPositionsFor(int ki) const
    {
        if (const auto it = m_targetableEnemiesByKi.find(ki);
            it != m_targetableEnemiesByKi.end() && !it->second.empty())
            return it->second;
        return m_enemyPositions;
    }

    int UnifiedTeamEvaluator::enemyPathDistance(int ki, TilePoint dest) const
    {
        if (ki >= 0 && ki < static_cast<int>(m_local.units.size()) &&
            m_local.width > 0 && m_local.height > 0 &&
            dest.x >= 0 && dest.x < m_local.width && dest.y >= 0 && dest.y < m_local.height)
        {
            if (const auto it = m_enemyPathDistByKi.find(ki); it != m_enemyPathDistByKi.end())
            {
                const std::size_t idx = static_cast<std::size_t>(dest.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(dest.x);
                if (idx < it->second->size() && (*it->second)[idx] < std::numeric_limits<std::int32_t>::max())
                    return (*it->second)[idx];
            }
        }
        return UNIFIED_UNREACHABLE_COST;
    }

    int UnifiedTeamEvaluator::repairPathDistance(int ki, TilePoint dest) const
    {
        if (ki >= 0 && ki < static_cast<int>(m_repairPathDistByKi.size()) &&
            m_local.width > 0 && m_local.height > 0 &&
            dest.x >= 0 && dest.x < m_local.width && dest.y >= 0 && dest.y < m_local.height)
        {
            const auto * distVec = m_repairPathDistByKi[static_cast<std::size_t>(ki)];
            const auto * maskVec = m_repairSiteMaskByKi[static_cast<std::size_t>(ki)];
            if (distVec == nullptr || maskVec == nullptr)
                return UNIFIED_UNREACHABLE_COST;
            const std::size_t cIdx = static_cast<std::size_t>(dest.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(dest.x);
            if (cIdx < maskVec->size() && (*maskVec)[cIdx] != 0)
                return 0;
            if (cIdx < distVec->size() && (*distVec)[cIdx] < std::numeric_limits<std::int32_t>::max())
                return (*distVec)[cIdx];
        }
        return UNIFIED_UNREACHABLE_COST;
    }

    int UnifiedTeamEvaluator::unownedPropertyPathDistance(int ki, TilePoint dest) const
    {
        const int closedDist = cachedSetPathDistance(m_unownedPropPathDistByGrid, m_unownedProperties, ki, dest);
        if (closedDist < UNIFIED_UNREACHABLE_COST || m_openPropPathDistByGrid.empty())
            return closedDist;
        if (ki >= 0 && ki < static_cast<int>(m_local.units.size()) &&
            m_local.width > 0 && m_local.height > 0 &&
            dest.x >= 0 && dest.x < m_local.width && dest.y >= 0 && dest.y < m_local.height)
        {
            const int gridId = pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]);
            if (const auto it = m_openPropPathDistByGrid.find(gridId); it != m_openPropPathDistByGrid.end())
            {
                const std::size_t idx = static_cast<std::size_t>(dest.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(dest.x);
                if (idx < it->second.size() && it->second[idx] < std::numeric_limits<std::int32_t>::max())
                    return m_maxDist + it->second[idx];
            }
        }
        return closedDist;
    }

    int UnifiedTeamEvaluator::unownedFactoryPathDistance(int ki, TilePoint dest) const
    {
        return cachedSetPathDistance(m_unownedFactoryPathDistByGrid, m_unownedFactories, ki, dest);
    }

    int UnifiedTeamEvaluator::combatObjectivePathDistance(int ki, TilePoint dest) const
    {
        if (!m_enemyOwnedProperties.empty())
        {
            const int ed = cachedSetPathDistance(m_enemyPropPathDistByGrid, m_enemyOwnedProperties, ki, dest);
            if (ed < UNIFIED_UNREACHABLE_COST)
                return ed;
        }
        return unownedPropertyPathDistance(ki, dest);
    }

    int UnifiedTeamEvaluator::gridTileDistance(int gridId, TilePoint from, TilePoint goal, bool allowAdjacent) const
    {
        if (from == goal) return 0;
        if (allowAdjacent && manhattanDistance(from, goal) <= 1) return 0;
        const int effGrid = (gridId >= 0 && gridId < static_cast<int>(m_local.grids.size()))
            ? gridId : (!m_local.grids.empty() ? 0 : -1);
        if (effGrid < 0 || m_local.width <= 0 || m_local.height <= 0 ||
            from.x < 0 || from.x >= m_local.width || from.y < 0 || from.y >= m_local.height ||
            goal.x < 0 || goal.x >= m_local.width || goal.y < 0 || goal.y >= m_local.height)
            return UNIFIED_UNREACHABLE_COST;
        const std::vector<std::int32_t> * distPtr = nullptr;
        const std::size_t goalCell = static_cast<std::size_t>(goal.y) * static_cast<std::size_t>(m_local.width) +
            static_cast<std::size_t>(goal.x);
        const std::size_t flatSlot = static_cast<std::size_t>(effGrid) * m_cellCount + goalCell;
        auto * flatTable = m_fast ? (allowAdjacent ? &m_fast->flatTileAdjDist : &m_fast->flatTileDist) : nullptr;
        if (flatTable != nullptr && flatSlot < flatTable->size())
            distPtr = (*flatTable)[flatSlot];
        if (distPtr == nullptr)
        {
            const PathDistKey key{effGrid, goal.x, goal.y, allowAdjacent ? 1 : 0, allowAdjacent ? 1 : 0};
            auto it = m_tilePathDistCache.find(key);
            if (it == m_tilePathDistCache.end())
            {
                const int range = allowAdjacent ? 1 : 0;
                it = m_tilePathDistCache.emplace(
                    key, buildReversePathDistances(effGrid, std::span<const TilePoint>(&goal, 1), range, range)).first;
            }
            distPtr = &it->second;
            if (flatTable != nullptr && flatSlot < flatTable->size())
                (*flatTable)[flatSlot] = distPtr;
        }
        const std::size_t idx = static_cast<std::size_t>(from.y) * static_cast<std::size_t>(m_local.width) +
            static_cast<std::size_t>(from.x);
        if (idx < distPtr->size() && (*distPtr)[idx] < std::numeric_limits<std::int32_t>::max())
            return (*distPtr)[idx];
        return UNIFIED_UNREACHABLE_COST;
    }

    int UnifiedTeamEvaluator::tilePathDistance(int ki, TilePoint from, TilePoint goal, bool allowAdjacent) const
    {
        if (from == goal) return 0;
        const int gridId = (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
            ? pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]) : -1;
        return gridTileDistance(gridId, from, goal, allowAdjacent);
    }

    int UnifiedTeamEvaluator::gridMoveCostForUnit(int ki, TilePoint from, TilePoint to) const
    {
        return tilePathDistance(ki, from, to, false);
    }

    bool UnifiedTeamEvaluator::enemyFactoryThreatensTile(TilePoint f, TilePoint tile) const
    {
        if (m_sampleCapperKi >= 0 && m_sampleCapperKi < static_cast<int>(m_local.units.size()))
        {
            const int move = std::max(1, m_local.units[static_cast<std::size_t>(m_sampleCapperKi)].movement);
            return tilePathDistance(m_sampleCapperKi, f, tile, true) <= move;
        }
        return false;
    }

    bool UnifiedTeamEvaluator::canSpawnReachAttack(int gridId, int moveBudget, int minRange, int maxRange,
                             TilePoint from, TilePoint target) const
    {
        const int effMin = std::max(1, minRange);
        const int effMax = std::max(effMin, maxRange);
        const int manh = manhattanDistance(from, target);
        if (manh >= effMin && manh <= effMax) return true;
        if (moveBudget <= 0) return false;
        const int effGrid = (gridId >= 0 && gridId < static_cast<int>(m_local.grids.size()))
            ? gridId : (!m_local.grids.empty() ? 0 : -1);
        if (effGrid < 0 || m_local.width <= 0 || m_local.height <= 0 ||
            from.x < 0 || from.x >= m_local.width || from.y < 0 || from.y >= m_local.height ||
            target.x < 0 || target.x >= m_local.width || target.y < 0 || target.y >= m_local.height)
            return false;
        const PathDistKey key{effGrid, target.x, target.y, effMin, effMax};
        auto it = m_indirectTilePathCache.find(key);
        if (it == m_indirectTilePathCache.end())
        {
            it = m_indirectTilePathCache.emplace(
                key, buildReversePathDistances(effGrid, std::span<const TilePoint>(&target, 1), effMin, effMax)).first;
        }
        const std::size_t idx = static_cast<std::size_t>(from.y) * static_cast<std::size_t>(m_local.width) +
            static_cast<std::size_t>(from.x);
        return idx < it->second.size() && it->second[idx] <= moveBudget;
    }

    std::pair<int, int> UnifiedTeamEvaluator::unitRangePublic(int ki) const
    {
        return unitAttackRange(ki);
    }

    int UnifiedTeamEvaluator::attackRingPathCostPublic(int ki, TilePoint from, TilePoint target, int minR, int maxR) const
    {
        return attackRingPathCost(ki, from, target, minR, maxR);
    }

    bool UnifiedTeamEvaluator::isJammedInReachableBlindSpot(int ownKi, TilePoint ownPos, int minR) const
    {
        if (minR <= 1 || ownKi < 0 || ownKi >= static_cast<int>(m_local.units.size()))
            return false;
        for (const auto & fe : m_forwardEnemies)
        {
            if (manhattanDistance(ownPos, fe.initialTile) >= minR)
                continue;
            const int eMove = fe.canMoveAndFire ? fe.move : 0;
            if (canUnitReachAttack(fe.ki, fe.initialTile, ownPos, eMove, fe.minRange, fe.maxRange) &&
                maxWeaponBaseDamage(fe.ki, ownKi, fe.minRange) > 0.0)
                return true;
        }
        return false;
    }

    bool UnifiedTeamEvaluator::canExertZocOn(int blockerKi, int moverKi) const
    {
        if (blockerKi < 0 || blockerKi >= static_cast<int>(m_local.units.size()) ||
            moverKi < 0 || moverKi >= static_cast<int>(m_local.units.size()))
            return false;
        const auto & blocker = m_local.units[static_cast<std::size_t>(blockerKi)];
        const auto & mover = m_local.units[static_cast<std::size_t>(moverKi)];
        if (blocker.ignoresCollision || mover.ignoresCollision || blocker.tile == INVALID_TILE)
            return false;
        const int moverGrid = pawnTravelGrid(mover);
        if (moverGrid < 0 || moverGrid >= static_cast<int>(m_local.grids.size()))
            return false;
        return tileEnterable(m_local.grids[static_cast<std::size_t>(moverGrid)], blocker.tile);
    }

    bool UnifiedTeamEvaluator::canScreenCaptureTile(int helperKi, TilePoint pos, TilePoint capDest,
                              std::span<const TilePoint> firingTiles) const
    {
        const auto [minR, maxR] = unitAttackRange(helperKi);
        const int manh = manhattanDistance(pos, capDest);
        if (minR > 1 && manh >= minR && manh <= maxR)
            return true;
        for (const TilePoint ft : firingTiles)
        {
            if (pos == ft)
                return true;
        }
        const int move = (helperKi >= 0 && helperKi < static_cast<int>(m_local.units.size()))
            ? std::max(1, m_local.units[static_cast<std::size_t>(helperKi)].movement) : 1;
        return tilePathDistance(helperKi, pos, capDest, true) <= move;
    }

    std::int64_t UnifiedTeamEvaluator::computeGeodesicProgress(std::span<const int> choices) const
    {
        std::int64_t progress = 0;
        for (const auto & member : m_problem.actors())
        {
            const int slot = member.canonicalSlot;
            if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
            const int opt = choices[static_cast<std::size_t>(slot)];
            if (opt < 0 || opt >= static_cast<int>(member.candidates.size())) continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(opt)];
            const int ki = member.knowledgeUnitIndex;
            const auto targets = enemyPositionsFor(ki);
            int d = 0;
            if (member.stockRow == NO_STOCK_ROW)
                d = !targets.empty() ? enemyPathDistance(ki, cand.destination) : combatObjectivePathDistance(ki, cand.destination);
            else if (!m_unownedProperties.empty())
                d = unownedPropertyPathDistance(ki, cand.destination);
            else if (!targets.empty())
                d = enemyPathDistance(ki, cand.destination);
            progress -= static_cast<std::int64_t>(d);
            if (member.stockRow == NO_STOCK_ROW && !targets.empty())
                progress += static_cast<std::int64_t>(indirectScreenContactCount(ki, cand.destination, choices));
        }
        return progress;
    }

    std::vector<CombatFireOrder> UnifiedTeamEvaluator::buildDefaultFireOrders(std::span<const int> choices) const
    {
        struct AttackerInfo
        {
            int unitId{NO_UNIT};
            MilliFunds counterCapital{0};
            std::int32_t damageSteps{0};
            bool diesToCounter{false};
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
            bool dies = false;
            if (comp && comp->kind == ComponentKind::Fire)
            {
                dmgSteps = comp->fire.damageSteps;
                const bool hasKnown = member.knowledgeUnitIndex >= 0 &&
                    member.knowledgeUnitIndex < static_cast<int>(m_knowledge.units().size());
                const bool indirect = hasKnown &&
                    m_knowledge.units()[static_cast<std::size_t>(member.knowledgeUnitIndex)].minRange > 1;
                if (!indirect && comp->fire.counterSteps > 0)
                {
                    const MilliFunds cost = member.knowledgeUnitIndex >= 0 &&
                        member.knowledgeUnitIndex < static_cast<int>(m_local.units.size())
                        ? m_local.units[static_cast<std::size_t>(member.knowledgeUnitIndex)].replacementCost
                        : cand.source.actor.replacementCost;
                    counterCap = bookDamage(cost, comp->fire.counterSteps);
                    const int hp = hasKnown
                        ? m_knowledge.units()[static_cast<std::size_t>(member.knowledgeUnitIndex)].hpRounded
                        : UNIT_HP_STEPS;
                    dies = hp > 0 && comp->fire.counterSteps >= hp;
                }
            }
            byTarget[cand.targetUnitId].push_back({member.engineUnitId, counterCap, dmgSteps, dies});
        }
        std::vector<CombatFireOrder> orders;
        for (auto & [targetId, attackers] : byTarget)
        {
            if (attackers.size() < 2) continue;
            std::stable_sort(attackers.begin(), attackers.end(), [](const AttackerInfo & a, const AttackerInfo & b) {
                const bool aDraws = a.counterCapital > 0;
                const bool bDraws = b.counterCapital > 0;
                if (aDraws != bDraws) return !aDraws;
                if (a.diesToCounter != b.diesToCounter) return !a.diesToCounter;
                if (aDraws && bDraws)
                {
                    const std::int64_t aEff = static_cast<std::int64_t>(a.damageSteps) * b.counterCapital;
                    const std::int64_t bEff = static_cast<std::int64_t>(b.damageSteps) * a.counterCapital;
                    if (aEff != bEff) return aEff > bEff;
                }
                if (a.damageSteps != b.damageSteps) return a.damageSteps > b.damageSteps;
                if (a.counterCapital != b.counterCapital) return a.counterCapital < b.counterCapital;
                return a.unitId < b.unitId;
            });
            for (std::size_t i = 0; i + 1 < attackers.size(); ++i)
                orders.push_back(CombatFireOrder{attackers[i].unitId, attackers[i + 1].unitId});
        }
        return orders;
    }

    UnifiedTeamScore UnifiedTeamEvaluator::evaluate(std::span<const int> choices, const UnifiedTeamScore * pruneFloor,
                              ReplyPassEvaluation * economicOut,
                              std::span<const CombatFireOrder> fireOrdersOverride,
                              bool useDefaultFireOrders)
    {
        UnifiedTeamScore summary;
        const std::vector<CombatFireOrder> defaultOrders = (fireOrdersOverride.empty() && useDefaultFireOrders)
            ? buildDefaultFireOrders(choices) : std::vector<CombatFireOrder>{};
        const std::span<const CombatFireOrder> activeFireOrders = !fireOrdersOverride.empty()
            ? fireOrdersOverride : std::span<const CombatFireOrder>(defaultOrders);
        const std::int64_t geoProgress = computeGeodesicProgress(choices);
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
        const auto exposedUnits = allExposedUnits(m_problem, m_local, choices);
        const auto exposedCappers = allExposedActiveCappers(m_problem, m_knowledge, m_local, choices);
        if (!exposedUnits.empty())
        {
            if (!exposedCappers.empty())
                policies.emplace_back(TacticalReply::InterruptCapture, NO_UNIT);
            for (const int uid : exposedUnits)
                policies.emplace_back(TacticalReply::TargetUnit, uid);
        }
        if (pruneFloor && pruneFloor->valid && pruneFloor->bindingPolicy != TacticalReply::Economic)
        {
            for (std::size_t pi = 1; pi < policies.size(); ++pi)
            {
                if (policies[pi].first == pruneFloor->bindingPolicy &&
                    policies[pi].second == pruneFloor->bindingTargetUnit)
                {
                    if (pi > 1)
                        std::rotate(policies.begin() + 1, policies.begin() + static_cast<std::ptrdiff_t>(pi),
                                    policies.begin() + static_cast<std::ptrdiff_t>(pi + 1));
                    break;
                }
            }
        }

        const auto sameEvalOutcome = [](const ReplyPassEvaluation & a, const ReplyPassEvaluation & b) {
            return a.enemyWaiting == b.enemyWaiting &&
                a.afterReply == b.afterReply &&
                a.combatLocations == b.combatLocations &&
                a.enemyReplyHp == b.enemyReplyHp &&
                a.enemyAfterOwn == b.enemyAfterOwn &&
                a.realAfterReply == b.realAfterReply &&
                a.shots == b.shots &&
                a.actors == b.actors;
        };
        struct CachedPolicyOutcome
        {
            ReplyPassEvaluation eval;
            UnifiedTeamScore score;
        };
        std::vector<CachedPolicyOutcome> cachedOutcomes;
        cachedOutcomes.reserve(policies.size());

        bool hasAny = false;
        for (const auto & [policy, targetId] : policies)
        {
            pumpUiEventsIfDue();
            if (m_work < m_replayTariff) return UnifiedTeamScore{};
            m_work -= m_replayTariff;
            ++m_evals;
            m_local.policy = policy;
            m_local.replyTarget = targetId;
            const auto t0 = m_profileEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto eval = evaluateReplyPassPlan(m_problem, choices, m_actingBoard, {}, activeFireOrders);
            if (m_profileEnabled)
                m_profReplyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!eval.valued() || !eval.replay.valid() || !eval.replay.complete())
                return UnifiedTeamScore{};
            if (policy == TacticalReply::Economic && economicOut)
                *economicOut = eval;

            std::optional<UnifiedTeamScore> reusedScore;
            for (const auto & cached : cachedOutcomes)
            {
                if (sameEvalOutcome(cached.eval, eval))
                {
                    reusedScore = cached.score;
                    reusedScore->bindingPolicy = policy;
                    reusedScore->bindingTargetUnit = targetId;
                    if (m_profileEnabled) ++m_profReused;
                    break;
                }
            }
            UnifiedTeamScore point;
            if (reusedScore.has_value())
            {
                point = *reusedScore;
            }
            else
            {
                auto tick = [&]() {
                    return m_profileEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                };
                auto addNs = [&](std::int64_t & acc, std::chrono::steady_clock::time_point start) {
                    if (m_profileEnabled)
                        acc += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                };
                if (m_profileEnabled) ++m_profScored;
                auto ts = tick();
                auto hqRank = m_hqRankFn(eval);
                addNs(m_profHqNs, ts);
                if (!hqRank) return UnifiedTeamScore{};
                ts = tick();
                const auto baseRealized = computeUnifiedRealizedMaterial(eval);
                addNs(m_profRealizedNs, ts);
                if (!baseRealized) return UnifiedTeamScore{};
                ts = tick();
                const auto [vacateLoss, vacateKills] = computeVacateRetaliationLoss(eval);
                addNs(m_profVacateNs, ts);
                ts = tick();
                const auto f1Result = computeF1RetaliationValue(eval, vacateKills);
                addNs(m_profF1Ns, ts);
                MilliFunds realized = *baseRealized;
                if (!RelativeStockDetail::checkedSubtract(realized, vacateLoss) ||
                    !RelativeStockDetail::checkedAdd(realized, f1Result.minimaxTotal))
                    return UnifiedTeamScore{};

                ts = tick();
                MilliFunds postStock = cachedPostReplyStock(eval, vacateKills, f1Result.deterredShooters);
                addNs(m_profStockNs, ts);
                ts = tick();
                MilliFunds forwardCov = computeForwardBoardCoverageValue(eval, &f1Result, vacateKills);
                addNs(m_profFwdCovNs, ts);
                ts = tick();
                const MilliFunds denial = computeEnemyDenial(eval);
                addNs(m_profDenialNs, ts);
                ts = tick();
                const auto [hqCaptureSafe, ownRisk] = computeOwnPropertyCaptureRisk(eval, vacateKills);
                addNs(m_profOwnRiskNs, ts);
                if (!hqCaptureSafe && std::get<0>(*hqRank) != TerminalClass::ForcedLoss)
                {
                    if (std::get<0>(*hqRank) != TerminalClass::ForcedWin)
                        std::get<0>(*hqRank) = TerminalClass::ForcedLoss;
                }
                ts = tick();
                const MilliFunds trapLoss = computeFollowupTrapLoss(eval, vacateKills);
                addNs(m_profTrapNs, ts);
                ts = tick();
                const int buildSlots = m_buildSlotsFn(eval);
                const MilliFunds factoryVal = computeProductionOpenSiteValue(eval);
                addNs(m_profFactoryNs, ts);
                ts = tick();
                const MilliFunds repairVal = computeRepairValue(eval, vacateKills, f1Result.deterredShooters);
                const MilliFunds breakableVal = computeBreakableTerrainValue(eval);
                addNs(m_profRepairBrkNs, ts);

                MilliFunds econTotal = realized;
                if (!RelativeStockDetail::checkedAdd(econTotal, postStock) ||
                    !RelativeStockDetail::checkedAdd(econTotal, forwardCov) ||
                    !RelativeStockDetail::checkedAdd(econTotal, denial) ||
                    !RelativeStockDetail::checkedSubtract(econTotal, ownRisk) ||
                    !RelativeStockDetail::checkedSubtract(econTotal, trapLoss) ||
                    !RelativeStockDetail::checkedAdd(econTotal, factoryVal) ||
                    !RelativeStockDetail::checkedAdd(econTotal, repairVal) ||
                    !RelativeStockDetail::checkedAdd(econTotal, breakableVal))
                    return UnifiedTeamScore{};

                if (!f1Result.deterredShooters.empty())
                {
                    F1RetaliationResult execF1;
                    execF1.total = f1Result.total;
                    execF1.minimaxTotal = f1Result.total;
                    execF1.enemyAfterF1 = f1Result.executedEnemyAfterF1;
                    execF1.usedFriendlies = f1Result.executedUsedFriendlies;
                    execF1.friendlyNextPos = f1Result.executedFriendlyNextPos;
                    MilliFunds realizedExec = *baseRealized;
                    if (!RelativeStockDetail::checkedSubtract(realizedExec, vacateLoss) ||
                        !RelativeStockDetail::checkedAdd(realizedExec, f1Result.total))
                        return UnifiedTeamScore{};
                    const MilliFunds postStockExec = cachedPostReplyStock(eval, vacateKills);
                    const MilliFunds forwardCovExec = computeForwardBoardCoverageValue(eval, &execF1, vacateKills);
                    const MilliFunds repairValExec = computeRepairValue(eval, vacateKills);
                    MilliFunds econExec = realizedExec;
                    if (!RelativeStockDetail::checkedAdd(econExec, postStockExec) ||
                        !RelativeStockDetail::checkedAdd(econExec, forwardCovExec) ||
                        !RelativeStockDetail::checkedAdd(econExec, denial) ||
                        !RelativeStockDetail::checkedSubtract(econExec, ownRisk) ||
                        !RelativeStockDetail::checkedSubtract(econExec, trapLoss) ||
                        !RelativeStockDetail::checkedAdd(econExec, factoryVal) ||
                        !RelativeStockDetail::checkedAdd(econExec, repairValExec) ||
                        !RelativeStockDetail::checkedAdd(econExec, breakableVal))
                        return UnifiedTeamScore{};
                    if (econExec < econTotal)
                    {
                        econTotal = econExec;
                        realized = realizedExec;
                        postStock = postStockExec;
                        forwardCov = forwardCovExec;
                    }
                }

                point.valid = true;
                point.worstHqRank = *hqRank;
                point.worstEconomicValue = econTotal;
                point.geodesicProgress = geoProgress;
                point.realizedMaterial = realized;
                point.postReplyStock = postStock;
                point.forwardCoverage = forwardCov;
                point.enemyDenial = denial;
                point.ownCaptureRisk = ownRisk;
                point.factoryValue = factoryVal;
                point.affordableBuildSlots = buildSlots;
                point.bindingPolicy = policy;
                point.bindingTargetUnit = targetId;
                cachedOutcomes.push_back({eval, point});
            }

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

    void UnifiedTeamEvaluator::refreshStockBaseline(const ReplyPassEvaluation & eval)
    {
        if (!m_stockContext.valid())
            return;
        const auto tuned = m_stockContext.tune(eval.afterReply, UNIFIED_STOCK_TUNE_ROUNDS);
        m_stockTuned = true;
        if (tuned.status == ReplyStockStatus::Valid && tuned.floor.status == ReplyStockStatus::Valid)
        {
            m_baselineFloor = tuned.floor;
            m_seqStockCache.clear();
            if (m_fast)
                m_fast->seqStockCache.clear();
        }
    }

    MilliFunds UnifiedTeamEvaluator::testVacateRetaliationLoss(const ReplyPassEvaluation & eval) const
    {
        return computeVacateRetaliationLoss(eval).first;
    }

    F1RetaliationResult UnifiedTeamEvaluator::testF1RetaliationResult(const ReplyPassEvaluation & eval) const
    {
        return computeF1RetaliationValue(eval);
    }

    MilliFunds UnifiedTeamEvaluator::testProductionOpenSiteValue(const ReplyPassEvaluation & eval) const
    {
        return computeProductionOpenSiteValue(eval);
    }

    int UnifiedTeamEvaluator::testEnemyPathDistance(int ki, TilePoint dest) const
    {
        return enemyPathDistance(ki, dest);
    }

    MilliFunds UnifiedTeamEvaluator::testBreakableTerrainValue(const ReplyPassEvaluation & eval) const
    {
        return computeBreakableTerrainValue(eval);
    }

    std::vector<std::uint8_t> UnifiedTeamEvaluator::testComputeBlockedStockNodes(const ReplyPassEvaluation & eval) const
    {
        return computeBlockedStockNodes(eval);
    }

    std::pair<bool, MilliFunds> UnifiedTeamEvaluator::testOwnPropertyCaptureRisk(const ReplyPassEvaluation & eval) const
    {
        return computeOwnPropertyCaptureRisk(eval);
    }

    std::int64_t UnifiedTeamEvaluator::testComputeGeodesicProgress(std::span<const int> choices) const
    {
        return computeGeodesicProgress(choices);
    }

    MilliFunds UnifiedTeamEvaluator::breakableTerrainStake(int targetId) const
    {
        const auto it = m_breakableProfiles.find(targetId);
        if (it == m_breakableProfiles.end() || it->second.surplusByBreachDelay.empty())
            return 0;
        return it->second.surplusByBreachDelay.front();
    }

    std::int32_t UnifiedTeamEvaluator::breakableTerrainHpSteps(int targetId) const
    {
        const auto it = m_breakableProfiles.find(targetId);
        return it == m_breakableProfiles.end() ? UNIT_HP_STEPS : it->second.initialHpSteps;
    }

    bool UnifiedTeamEvaluator::canMoverReachFiringSquare(int moverKi, TilePoint from, TilePoint sq, int moveBudget,
                                   std::uint32_t occGen, TilePoint ignoredTile,
                                   const std::vector<std::uint32_t> & genGrid,
                                   const std::vector<int> & kiGrid) const
    {
        if (m_local.width <= 0 || m_local.height <= 0 ||
            from.x < 0 || from.x >= m_local.width || from.y < 0 || from.y >= m_local.height ||
            sq.x < 0 || sq.x >= m_local.width || sq.y < 0 || sq.y >= m_local.height)
            return false;
        if (from == sq)
            return true;
        const std::size_t sqIdx = static_cast<std::size_t>(sq.y) * static_cast<std::size_t>(m_local.width) +
            static_cast<std::size_t>(sq.x);
        if (occGen != 0 && genGrid[sqIdx] == occGen && sq != ignoredTile)
            return false;
        if (moveBudget <= 0)
            return false;
        const int gridId = (moverKi >= 0 && moverKi < static_cast<int>(m_local.units.size()))
            ? pawnTravelGrid(m_local.units[static_cast<std::size_t>(moverKi)]) : -1;
        const int effGrid = (gridId >= 0 && gridId < static_cast<int>(m_local.grids.size()))
            ? gridId : (!m_local.grids.empty() ? 0 : -1);
        if (effGrid < 0)
            return false;
        if (occGen == 0)
            return gridTileDistance(effGrid, from, sq, false) <= moveBudget;
        return boundedCanReachGoalOnOcc(moverKi, effGrid, from, sq, moveBudget, occGen, ignoredTile, genGrid, kiGrid);
    }

    bool UnifiedTeamEvaluator::canFriendlyReachFiringSquare(int ki, TilePoint from, TilePoint sq, int moveBudget,
                                      std::uint32_t occEnemyGen) const
    {
        return canMoverReachFiringSquare(ki, from, sq, moveBudget, occEnemyGen, INVALID_TILE,
                                         m_occEnemyGen, m_occEnemyKi);
    }

    bool UnifiedTeamEvaluator::canEnemyReachFiringSquare(int eKi, TilePoint from, TilePoint sq, int moveBudget,
                                   std::uint32_t occFriendlyGen, TilePoint ignoredFriendlyTile) const
    {
        return canMoverReachFiringSquare(eKi, from, sq, moveBudget, occFriendlyGen, ignoredFriendlyTile,
                                         m_occFriendlyGen, m_occFriendlyKi);
    }

    bool UnifiedTeamEvaluator::canMoverReachAttackOccupied(int moverKi, TilePoint from, TilePoint target, int moveBudget,
                                     int minRange, int maxRange, std::uint32_t occGen, TilePoint ignoredTile,
                                     const std::vector<std::uint32_t> & genGrid,
                                     const std::vector<int> & kiGrid) const
    {
        const int effMin = std::max(1, minRange);
        const int effMax = std::max(effMin, maxRange);
        const int manh = manhattanDistance(from, target);
        if (manh >= effMin && manh <= effMax)
            return true;
        if (moveBudget <= 0)
            return false;
        if (occGen == 0)
            return canUnitReachAttack(moverKi, from, target, moveBudget, effMin, effMax);
        const int x0 = std::max(0, target.x - effMax);
        const int x1 = std::min(m_local.width - 1, target.x + effMax);
        const int y0 = std::max(0, target.y - effMax);
        const int y1 = std::min(m_local.height - 1, target.y + effMax);
        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                const TilePoint sq{x, y};
                const int d = manhattanDistance(sq, target);
                if (d < effMin || d > effMax)
                    continue;
                if (canMoverReachFiringSquare(moverKi, from, sq, moveBudget, occGen, ignoredTile, genGrid, kiGrid))
                    return true;
            }
        }
        return false;
    }

    bool UnifiedTeamEvaluator::canFriendlyReachAttackOccupied(int ki, TilePoint from, TilePoint target, int moveBudget,
                                        int minRange, int maxRange, std::uint32_t occEnemyGen) const
    {
        return canMoverReachAttackOccupied(ki, from, target, moveBudget, minRange, maxRange,
                                           occEnemyGen, INVALID_TILE, m_occEnemyGen, m_occEnemyKi);
    }

    bool UnifiedTeamEvaluator::canEnemyReachAttackOccupied(int eKi, TilePoint from, TilePoint target, int moveBudget,
                                     int minRange, int maxRange, std::uint32_t occFriendlyGen,
                                     TilePoint ignoredFriendlyTile) const
    {
        return canMoverReachAttackOccupied(eKi, from, target, moveBudget, minRange, maxRange,
                                           occFriendlyGen, ignoredFriendlyTile, m_occFriendlyGen, m_occFriendlyKi);
    }

    std::optional<double> UnifiedTeamEvaluator::defenderHpAfterCover(const CaptureCoverActor & fc, const CaptureCoverActor & d,
                                               TilePoint tile, std::uint32_t occEnemyGen,
                                               std::uint32_t occFriendlyGen) const
    {
        const double coverBase = maxWeaponBaseDamage(fc.ki, d.ki, fc.strikeRange);
        if (coverBase <= 0.0) return std::nullopt;
        const bool coverReaches =
            canFriendlyReachAttackOccupied(fc.ki, fc.pos, tile, fc.moveBudget, fc.strikeRange, fc.maxRange, occEnemyGen) ||
            (d.pos != INVALID_TILE &&
             canFriendlyReachAttackOccupied(fc.ki, fc.pos, d.pos, fc.moveBudget, fc.strikeRange, fc.maxRange, occEnemyGen));
        if (!coverReaches)
            return std::nullopt;
        const bool defenderHitsCover =
            canEnemyReachAttackOccupied(d.ki, d.pos, fc.pos, d.moveBudget, d.strikeRange, d.maxRange, occFriendlyGen);
        if (fc.strikeRange > 1)
        {
            // Exposed indirects have no counterattack and cannot cover their own blind spot.
            if (defenderHitsCover)
                return std::nullopt;
            if (d.strikeRange == 1)
            {
                for (const EntryDirection dir : ENTRY_DIRECTIONS)
                {
                    const StepVector step = entryStep(dir);
                    const TilePoint adj{tile.x + step.dx, tile.y + step.dy};
                    if (adj.x < 0 || adj.x >= m_local.width || adj.y < 0 || adj.y >= m_local.height)
                        continue;
                    if (canEnemyReachFiringSquare(d.ki, d.pos, adj, d.moveBudget, occFriendlyGen) &&
                        manhattanDistance(fc.pos, adj) < fc.strikeRange)
                    {
                        return std::nullopt;
                    }
                }
            }
        }
        const TerrainDefenseRule dDef = m_local.defense
            ? m_local.defense(d.ki, d.pos) : TerrainDefenseRule{};
        const double rawHit = terrainReducedDamage(dDef, coverBase, fc.hp, ceilHpSteps(d.hp));
        const double hit = engineShotDamage(rawHit);
        if (hit <= 0.0) return std::nullopt;
        const double afterD = engineHpAfterReply(d.hp, hit);
        if (afterD <= 0.0) return 0.0;
        const MilliFunds capLoss = combatCapitalLoss(d.cost, d.hp, afterD);
        MilliFunds counterLoss = 0;
        bool friendlySurvives = true;
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
                friendlySurvives = afterF > 0.0;
                counterLoss = combatCapitalLoss(fc.cost, fc.hp, afterF);
            }
        }
        if (!friendlySurvives || ((d.cost != 0 || fc.cost != 0) && capLoss <= counterLoss))
            return std::nullopt;
        if (fc.strikeRange == 1 && defenderHitsCover)
        {
            const double dStrike = maxWeaponBaseDamage(d.ki, fc.ki, d.strikeRange);
            if (dStrike > 0.0)
            {
                const TerrainDefenseRule fDef = m_local.defense
                    ? m_local.defense(fc.ki, fc.pos) : TerrainDefenseRule{};
                const double rawStrike = terrainReducedDamage(fDef, dStrike, d.hp, ceilHpSteps(fc.hp));
                const double strikeHit = engineShotDamage(rawStrike);
                const double fcAfter = engineHpAfterReply(fc.hp, strikeHit);
                const MilliFunds fcLoss = combatCapitalLoss(fc.cost, fc.hp, fcAfter);
                MilliFunds dCounterLoss = 0;
                if (d.strikeRange == 1 && fcAfter > 0.0)
                {
                    const double fcRet = maxWeaponBaseDamage(fc.ki, d.ki, 1);
                    if (fcRet > 0.0)
                    {
                        const double rawRet = sameActionCounterDamage(
                            dDef, fcRet, fc.hp, rawStrike, ceilHpSteps(d.hp));
                        dCounterLoss = combatCapitalLoss(
                            d.cost, d.hp, engineHpAfterReply(d.hp, engineShotDamage(rawRet)));
                    }
                }
                const MilliFunds dNetOnFc = fcLoss - dCounterLoss;
                if ((capLoss - counterLoss) < dNetOnFc)
                    return std::nullopt;
            }
        }
        return afterD;
    }

    bool UnifiedTeamEvaluator::isCaptureTileCovered(const CaptureCoverActor & fc, const CaptureCoverActor & d, TilePoint tile,
                              std::uint32_t occEnemyGen, std::uint32_t occFriendlyGen) const
    {
        return defenderHpAfterCover(fc, d, tile, occEnemyGen, occFriendlyGen).has_value();
    }

    bool UnifiedTeamEvaluator::testCaptureTileCovered(const CaptureCoverActor & fc, const CaptureCoverActor & d, TilePoint tile) const
    {
        return isCaptureTileCovered(fc, d, tile);
    }

    std::vector<std::uint8_t> UnifiedTeamEvaluator::testBlockedStockNodes(const ReplyPassEvaluation & eval) const
    {
        return computeBlockedStockNodes(eval);
    }

    MilliFunds UnifiedTeamEvaluator::testFollowupTrapLoss(const ReplyPassEvaluation & eval) const
    {
        return computeFollowupTrapLoss(eval);
    }

    std::optional<MilliFunds> UnifiedTeamEvaluator::testUnifiedRealizedMaterial(const ReplyPassEvaluation & eval) const
    {
        return computeUnifiedRealizedMaterial(eval);
    }

    MilliFunds UnifiedTeamEvaluator::testF1RetaliationValue(const ReplyPassEvaluation & eval) const
    {
        return computeF1RetaliationValue(eval).total;
    }

    MilliFunds UnifiedTeamEvaluator::testRepairValue(const ReplyPassEvaluation & eval) const
    {
        return computeRepairValue(eval);
    }

    MilliFunds UnifiedTeamEvaluator::testForwardBoardCoverageValue(const ReplyPassEvaluation & eval) const
    {
        return computeForwardBoardCoverageValue(eval);
    }

    bool UnifiedTeamEvaluator::testIsJammedInReachableBlindSpot(int ownKi, TilePoint ownPos, int minR) const
    {
        return isJammedInReachableBlindSpot(ownKi, ownPos, minR);
    }

    MilliFunds UnifiedTeamEvaluator::propertyOneTurnSwingAt(TilePoint tile) const
    {
        const int horizon = m_stockField.horizonTurns() > 0
            ? m_stockField.horizonTurns() : DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
        MilliFunds swing = 0;
        for (const auto & prop : m_properties)
        {
            if (prop.x == tile.x && prop.y == tile.y)
            {
                const Relation rel = m_knowledge.relation(prop.ownerId);
                const bool isEnemy = prop.ownerId != NO_OWNER &&
                    (m_knowledge.observerId() == NO_OWNER ? true : rel == Relation::Enemy);
                const OwnerSign beforeSign = prop.ownerId == NO_OWNER
                    ? OwnerSign::Neutral : (isEnemy ? OwnerSign::Enemy : OwnerSign::Ours);
                swing = std::max<MilliFunds>(0, incomeSwing(captureIncome(prop), 1, beforeSign, OwnerSign::Ours));
                break;
            }
        }
        if (swing <= 0)
        {
            if (const int colSlot = m_stockField.columnSlotAt(tile); colSlot != NO_STOCK_COLUMN)
            {
                const auto col = mirroredColumn(m_stockField.enemyColumn(colSlot));
                swing = std::max<MilliFunds>(0, incomeSwing(col.income, 1, col.ownerBefore, OwnerSign::Ours));
            }
        }
        if (m_productionModel != nullptr && m_productionModel->valid() && horizon > 1)
        {
            const MilliFunds bonus =
                m_productionModel->combatBonusCaptureValue(tile, 0, horizon) +
                m_productionModel->siteCaptureProductionSurplus(tile, 0, horizon);
            swing += bonus / horizon;
        }
        return swing;
    }

    void UnifiedTeamEvaluator::precomputeForwardEnemies()
    {
        m_forwardEnemies.clear();
        if ((!m_local.weapon && !m_local.weaponSlots) || m_enemyUnits.empty())
            return;
        m_forwardEnemies.reserve(m_enemyUnits.size());
        for (const auto & [eKi, eTile] : m_enemyUnits)
        {
            const auto & ep = m_local.units[static_cast<std::size_t>(eKi)];
            if (!ep.present || !ep.enemy || ep.replacementCost <= 0 || ep.tile == INVALID_TILE)
                continue;
            const auto [eMinR, eMaxR] = unitAttackRange(eKi);
            const bool canMoveFire = eMinR <= 1;
            bool screenableByDirect = canMoveFire && !ep.ignoresCollision;
            if (m_productionModel != nullptr && m_productionModel->valid())
            {
                for (const auto & pe : m_productionModel->input().enemies)
                {
                    if (pe.knowledgeUnitIndex == eKi)
                    {
                        screenableByDirect = canMoveFire && !pe.bypassesGroundScreen;
                        break;
                    }
                }
            }
            std::int32_t adjOpen = UNIFIED_PROD_OPEN_ADJACENT_SLOTS;
            const int eGrid = pawnTravelGrid(ep);
            if (eGrid >= 0 && eGrid < static_cast<int>(m_local.grids.size()) && screenableByDirect)
            {
                const auto & grid = m_local.grids[static_cast<std::size_t>(eGrid)];
                std::int32_t openCount = 0;
                for (const EntryDirection dir : ENTRY_DIRECTIONS)
                {
                    const StepVector step = entryStep(dir);
                    const int ax = eTile.x + step.dx;
                    const int ay = eTile.y + step.dy;
                    if (grid.onGrid(ax, ay) && tileEnterable(grid, {ax, ay}))
                        ++openCount;
                }
                adjOpen = std::max<std::int32_t>(1, openCount);
            }
            m_forwardEnemies.push_back(ForwardEnemySummary{
                .ki = eKi,
                .initialTile = eTile,
                .cost = ep.replacementCost,
                .move = std::max(0, ep.movement),
                .minRange = eMinR,
                .maxRange = eMaxR,
                .directAttackSlots = adjOpen,
                .canMoveAndFire = canMoveFire,
                .screenableByDirect = screenableByDirect,
                .capper = ep.capper,
                .fullFirstStrikeThreat = 0,
                .fullCapThreatPerTurn = 0,
            });
        }
    }

    int UnifiedTeamEvaluator::attackRingPathCost(int ki, TilePoint from, TilePoint target, int minR, int maxR) const
    {
        const int effMin = std::max(1, minR);
        const int effMax = std::max(effMin, maxR);
        const int gridId = (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
            ? pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]) : -1;
        const int effGrid = (gridId >= 0 && gridId < static_cast<int>(m_local.grids.size()))
            ? gridId : (!m_local.grids.empty() ? 0 : -1);
        if (effGrid < 0 || m_local.width <= 0 || m_local.height <= 0 ||
            from.x < 0 || from.x >= m_local.width || from.y < 0 || from.y >= m_local.height ||
            target.x < 0 || target.x >= m_local.width || target.y < 0 || target.y >= m_local.height)
            return UNIFIED_UNREACHABLE_COST;
        const int md = manhattanDistance(from, target);
        if (md >= effMin && md <= effMax &&
            tileEnterable(m_local.grids[static_cast<std::size_t>(effGrid)], from))
            return 0;
        const std::vector<std::int32_t> * distPtr = nullptr;
        const bool isMeleeRing = (effMin == 1 && effMax == 1);
        const std::size_t flatSlot = isMeleeRing
            ? (static_cast<std::size_t>(effGrid) * m_cellCount +
               static_cast<std::size_t>(target.y) * static_cast<std::size_t>(m_local.width) +
               static_cast<std::size_t>(target.x))
            : std::numeric_limits<std::size_t>::max();
        if (isMeleeRing && m_fast && flatSlot < m_fast->flatMeleeRingDist.size())
            distPtr = m_fast->flatMeleeRingDist[flatSlot];
        if (distPtr == nullptr)
        {
            const PathDistKey key{effGrid, target.x, target.y, effMin, effMax};
            auto it = m_indirectTilePathCache.find(key);
            if (it == m_indirectTilePathCache.end())
            {
                it = m_indirectTilePathCache.emplace(
                    key, buildReversePathDistances(effGrid, std::span<const TilePoint>(&target, 1), effMin, effMax)).first;
            }
            distPtr = &it->second;
            if (isMeleeRing && m_fast && flatSlot < m_fast->flatMeleeRingDist.size())
                m_fast->flatMeleeRingDist[flatSlot] = distPtr;
        }
        const std::size_t idx = static_cast<std::size_t>(from.y) * static_cast<std::size_t>(m_local.width) +
            static_cast<std::size_t>(from.x);
        if (idx < distPtr->size() && (*distPtr)[idx] < std::numeric_limits<std::int32_t>::max())
            return (*distPtr)[idx];
        return UNIFIED_UNREACHABLE_COST;
    }

    TerrainDefenseRule UnifiedTeamEvaluator::bestMeleeFiringDefense(int ki, TilePoint from, TilePoint target,
                                              int move, int maxCostBudget) const
    {
        if (!m_local.defense)
            return TerrainDefenseRule{};
        if (move <= 0)
            return m_local.defense(ki, from);
        const int gridId = (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
            ? pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]) : -1;
        const int effGrid = (gridId >= 0 && gridId < static_cast<int>(m_local.grids.size()))
            ? gridId : (!m_local.grids.empty() ? 0 : -1);
        bool foundReachable = false;
        TerrainDefenseRule bestReach{};
        bool foundAny = false;
        TerrainDefenseRule bestAny{};
        for (const EntryDirection dir : ENTRY_DIRECTIONS)
        {
            const StepVector step = entryStep(dir);
            const TilePoint adj{target.x + step.dx, target.y + step.dy};
            if (adj.x < 0 || adj.x >= m_local.width || adj.y < 0 || adj.y >= m_local.height)
                continue;
            if (effGrid >= 0 && !tileEnterable(m_local.grids[static_cast<std::size_t>(effGrid)], adj))
                continue;
            const TerrainDefenseRule def = m_local.defense(ki, adj);
            if (!foundAny || def.stars > bestAny.stars)
            {
                foundAny = true;
                bestAny = def;
            }
            if ((!foundReachable || def.stars > bestReach.stars) &&
                (adj == from || gridTileDistance(effGrid, from, adj, false) <= maxCostBudget))
            {
                foundReachable = true;
                bestReach = def;
            }
        }
        if (foundReachable)
            return bestReach;
        if (foundAny)
            return bestAny;
        return m_local.defense(ki, from);
    }

    MilliFunds UnifiedTeamEvaluator::computeForwardBoardCoverageValue(
        const ReplyPassEvaluation & eval,
        const F1RetaliationResult * f1Res,
        std::span<const int> vacateKills) const
    {
        if (m_forwardEnemies.empty() || (!m_local.weapon && !m_local.weaponSlots))
            return 0;
        const std::int32_t horizon = m_stockField.horizonTurns() > 0
            ? m_stockField.horizonTurns() : DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
        if (horizon <= 1)
            return 0;

        struct LiveEnemyState
        {
            const ForwardEnemySummary * info{nullptr};
            TilePoint pos{INVALID_TILE};
            double hp{0.0};
            MilliFunds liveCap{0};
            bool firedInE0{false};
            bool usedInF1Ret{false};
            bool fleeingE0{false};
        };
        const auto isDeterredShooter = [&](int shooterKi) {
            return f1Res != nullptr &&
                std::ranges::find(f1Res->deterredShooters, shooterKi) != f1Res->deterredShooters.end();
        };
        std::map<int, int> grantedByTarget;
        if (!m_local.ownFire)
        {
            for (const auto & actor : eval.actors)
            {
                if (actor.grantedDamage <= 0 || actor.ownShot.target != NO_UNIT)
                    continue;
                if (actor.actorSlot < 0 || actor.actorSlot >= static_cast<int>(m_problem.actors().size()))
                    continue;
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                if (actor.candidateIndex < 0 || actor.candidateIndex >= static_cast<int>(member.candidates.size()))
                    continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                for (const auto & comp : cand.source.bundle.components)
                {
                    if (comp.kind == ComponentKind::Fire && !isTerrainTargetId(comp.fire.targetUnitId))
                    {
                        grantedByTarget[comp.fire.targetUnitId] += actor.grantedDamage;
                        break;
                    }
                }
            }
        }
        std::vector<LiveEnemyState> liveEnemies;
        liveEnemies.reserve(m_forwardEnemies.size());
        for (const auto & fe : m_forwardEnemies)
        {
            const auto & ep = m_local.units[static_cast<std::size_t>(fe.ki)];
            const bool deterredE0 = isDeterredShooter(fe.ki);
            double hpE = ep.realHp >= 0.0
                ? ep.realHp
                : (fe.ki < static_cast<int>(m_knowledge.units().size())
                       ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(fe.ki)].hpRounded)
                       : UNIFIED_HP_STEPS_DOUBLE);
            if (!deterredE0 && eval.enemyReplyHp.contains(fe.ki))
                hpE = static_cast<double>(eval.enemyReplyHp.at(fe.ki));
            else if (const auto itOwn = eval.enemyAfterOwn.find(fe.ki); itOwn != eval.enemyAfterOwn.end())
                hpE = static_cast<double>(itOwn->second);
            else if (const auto itGranted = grantedByTarget.find(fe.ki); itGranted != grantedByTarget.end())
                hpE = std::max(0.0, hpE - static_cast<double>(itGranted->second));
            bool usedF1 = false;
            if (f1Res != nullptr)
            {
                if (const auto itF1 = f1Res->enemyAfterF1.find(fe.ki); itF1 != f1Res->enemyAfterF1.end())
                {
                    hpE = itF1->second;
                    usedF1 = true;
                }
            }
            if (hpE <= 0.0)
                continue;
            TilePoint ePos = (!deterredE0 && fe.ki < static_cast<int>(eval.combatLocations.size()) &&
                              eval.combatLocations[static_cast<std::size_t>(fe.ki)] != INVALID_TILE)
                ? eval.combatLocations[static_cast<std::size_t>(fe.ki)]
                : fe.initialTile;
            bool firedE0 = false;
            if (!deterredE0)
            {
                for (const ReplyPassShot & s : eval.shots)
                {
                    if (s.shooterIndex == fe.ki)
                    {
                        firedE0 = true;
                        if (s.firingTile != INVALID_TILE)
                            ePos = s.firingTile;
                        break;
                    }
                }
            }
            const MilliFunds liveCap = combatCapitalLoss(fe.cost, hpE, 0.0);
            liveEnemies.push_back({&fe, ePos, hpE, liveCap, firedE0, usedF1, false});
        }
        if (liveEnemies.empty())
            return 0;

        struct FriendlyForwardUnit
        {
            int ki{NO_UNIT};
            TilePoint pos{INVALID_TILE};
            double hp{0.0};
            int move{0};
            int minR{1};
            int maxR{1};
            MilliFunds cost{0};
            bool directCombat{false};
            int readyTurn{1};
        };
        std::vector<FriendlyForwardUnit> friendlies;
        std::vector<std::uint8_t> seenKi(m_local.units.size(), 0);
        const auto onlyHitByDeterredShooters = [&](int targetKi, double & preDeterredHpOut) {
            if (f1Res == nullptr || f1Res->deterredShooters.empty())
                return false;
            bool anyShot = false;
            double earliestBefore = -1.0;
            for (const ReplyPassShot & s : eval.shots)
            {
                if (s.targetKnowledge != targetKi)
                    continue;
                if (!isDeterredShooter(s.shooterIndex))
                    return false;
                if (!anyShot)
                    earliestBefore = s.realBefore;
                anyShot = true;
            }
            if (anyShot && earliestBefore > 0.0)
            {
                preDeterredHpOut = earliestBefore;
                return true;
            }
            return false;
        };
        const auto addFriendly = [&](int ki, TilePoint pos, double hpF, int readyTurn) {
            if (hpF <= 0.0 || pos == INVALID_TILE)
                return;
            if (ki < 0 || ki >= static_cast<int>(m_local.units.size()))
                return;
            if (std::ranges::find(vacateKills, ki) != vacateKills.end())
                return;
            int effReadyTurn = std::max(1, readyTurn);
            TilePoint effPos = pos;
            if (f1Res != nullptr && std::ranges::find(f1Res->usedFriendlies, ki) != f1Res->usedFriendlies.end())
            {
                effReadyTurn = std::max(effReadyTurn, 2);
                if (const auto itNext = f1Res->friendlyNextPos.find(ki);
                    itNext != f1Res->friendlyNextPos.end() && itNext->second != INVALID_TILE)
                    effPos = itNext->second;
            }
            if (effReadyTurn >= horizon)
                return;
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            if (!fp.present || fp.enemy || fp.replacementCost <= 0)
                return;
            const auto [minR, maxR] = unitAttackRange(ki);
            if (ki < static_cast<int>(m_knowledge.units().size()) &&
                !m_knowledge.units()[static_cast<std::size_t>(ki)].hasWeapons)
                return;
            friendlies.push_back({ki, effPos, hpF, std::max(0, fp.movement), minR, maxR,
                                  fp.replacementCost, minR <= 1 && fp.movement > 0, effReadyTurn});
        };
        for (const auto & actor : eval.actors)
        {
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const int ki = member.knowledgeUnitIndex;
            if (ki >= 0 && ki < static_cast<int>(seenKi.size()))
                seenKi[static_cast<std::size_t>(ki)] = 1;
            double restoredHp = 0.0;
            const bool restored = onlyHitByDeterredShooters(ki, restoredHp);
            if (!restored && actor.stepsAfterReply <= 0)
                continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
            double hpF = restored ? restoredHp : static_cast<double>(actor.stepsAfterReply);
            if (!restored)
            {
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    hpF = it->second;
            }
            const int readyT = cand.capture.has_value()
                ? std::max(1, cand.capture->turnsUntilOwned + 1)
                : 1;
            addFriendly(ki, cand.destination, hpF, readyT);
        }
        for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
        {
            if (seenKi[static_cast<std::size_t>(ki)] != 0)
                continue;
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            if (!fp.present || fp.enemy || fp.tile == INVALID_TILE)
                continue;
            double restoredHp = 0.0;
            double hpF = fp.realHp >= 0.0
                ? fp.realHp
                : (ki < static_cast<int>(m_knowledge.units().size())
                       ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded)
                       : UNIFIED_HP_STEPS_DOUBLE);
            if (onlyHitByDeterredShooters(ki, restoredHp))
                hpF = restoredHp;
            else if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                hpF = it->second;
            addFriendly(ki, fp.tile, hpF, 1);
        }

        const std::size_t enemyCount = liveEnemies.size();
        const std::size_t friendlyCount = friendlies.size();

        const std::uint32_t occFriendlyGen = nextOccFriendlyGen();
        for (const auto & u : friendlies)
        {
            if (u.pos.x >= 0 && u.pos.x < m_local.width && u.pos.y >= 0 && u.pos.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(u.pos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(u.pos.x);
                m_occFriendlyGen[cIdx] = occFriendlyGen;
                m_occFriendlyKi[cIdx] = u.ki;
            }
        }
        const std::uint32_t occEnemyGen = nextOccEnemyGen();
        for (const auto & le : liveEnemies)
        {
            if (le.pos.x >= 0 && le.pos.x < m_local.width && le.pos.y >= 0 && le.pos.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(le.pos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(le.pos.x);
                m_occEnemyGen[cIdx] = occEnemyGen;
                m_occEnemyKi[cIdx] = le.info->ki;
            }
        }

        // Unpinned enemies with no favorable E0 shot retreat on E0 rather than waiting in F1 range.
        for (std::size_t ei = 0; ei < enemyCount; ++ei)
        {
            auto & le = liveEnemies[ei];
            if (le.firedInE0 || le.usedInF1Ret || le.info->move <= 0)
                continue;
            if (le.info->ki >= 0 && le.info->ki < static_cast<int>(m_knowledge.units().size()) &&
                m_knowledge.units()[static_cast<std::size_t>(le.info->ki)].capturePoints > 0)
                continue;
            const FriendlyForwardUnit * threateningU = nullptr;
            for (const auto & u : friendlies)
            {
                if (u.readyTurn > 1)
                    continue;
                const double bDmg = maxWeaponBaseDamage(u.ki, le.info->ki, u.minR);
                if (bDmg <= 0.0)
                    continue;
                const int mc = attackRingPathCost(u.ki, u.pos, le.pos, u.minR, u.maxR);
                const bool hitsF1 = u.minR <= 1
                    ? (mc == 0 || (u.move > 0 && mc <= u.move))
                    : (mc == 0 && manhattanDistance(u.pos, le.pos) >= u.minR && manhattanDistance(u.pos, le.pos) <= u.maxR);
                if (!hitsF1)
                    continue;
                const TerrainDefenseRule eDef = m_local.defense ? m_local.defense(le.info->ki, le.pos) : TerrainDefenseRule{};
                const double rawH = terrainReducedDamage(eDef, bDmg, u.hp, ceilHpSteps(le.hp));
                const double h = engineShotDamage(rawH);
                if (h <= 0.0)
                    continue;
                const double eAfter = engineHpAfterReply(le.hp, h);
                const MilliFunds eLoss = combatCapitalLoss(le.info->cost, le.hp, eAfter);
                MilliFunds uCtr = 0;
                if (u.minR == 1 && le.info->minRange == 1 && eAfter > 0.0)
                {
                    const double retD = maxWeaponBaseDamage(le.info->ki, u.ki, 1);
                    if (retD > 0.0)
                    {
                        const TerrainDefenseRule fDef = bestMeleeFiringDefense(u.ki, u.pos, le.pos, u.move, std::max(1, u.move));
                        const double rawC = sameActionCounterDamage(fDef, retD, u.hp, rawH, ceilHpSteps(le.hp));
                        uCtr = combatCapitalLoss(u.cost, u.hp, engineHpAfterReply(u.hp, engineShotDamage(rawC)));
                    }
                }
                if (eLoss > uCtr)
                {
                    threateningU = &u;
                    break;
                }
            }
            if (threateningU == nullptr)
                continue;
            const auto & ep = m_local.units[static_cast<std::size_t>(le.info->ki)];
            const int eGrid = pawnTravelGrid(ep);
            if (eGrid < 0 || eGrid >= static_cast<int>(m_local.grids.size()))
                continue;
            collectEnemyEscapeTiles(le.info->ki, eGrid, le.pos, le.info->move, ep.ignoresCollision,
                                    occFriendlyGen, occEnemyGen);
            if (m_escapeTilesScratch.size() <= 1)
                continue;
            std::vector<const FriendlyForwardUnit *> damagingFriendlies;
            damagingFriendlies.reserve(friendlies.size());
            // Put the F1 threatener first so monotonic pruning triggers immediately on tiles still in its range.
            damagingFriendlies.push_back(threateningU);
            for (const auto & u : friendlies)
            {
                if (&u != threateningU && maxWeaponBaseDamage(u.ki, le.info->ki, u.minR) > 0.0)
                    damagingFriendlies.push_back(&u);
            }
            const auto escapeScore = [&](TilePoint q, int floorTurn, int floorCost, bool & pruned) {
                pruned = false;
                int minTurn = horizon;
                int minCost = UNIFIED_UNREACHABLE_COST;
                for (const FriendlyForwardUnit * up : damagingFriendlies)
                {
                    const auto & u = *up;
                    const int mc = attackRingPathCost(u.ki, u.pos, q, u.minR, u.maxR);
                    if (mc >= UNIFIED_UNREACHABLE_COST)
                        continue;
                    minCost = std::min(minCost, mc);
                    int ft = horizon;
                    if (u.minR <= 1)
                    {
                        if (u.move <= 0 && mc > 0)
                            continue;
                        const int mt = u.move > 0 ? (mc + u.move - 1) / u.move : 0;
                        ft = std::max(u.readyTurn, u.readyTurn - 1 + mt);
                    }
                    else if (u.readyTurn == 1 && mc == 0 &&
                             manhattanDistance(u.pos, q) >= u.minR && manhattanDistance(u.pos, q) <= u.maxR)
                    {
                        ft = 1;
                    }
                    else if (u.move > 0)
                    {
                        ft = u.readyTurn + (std::max(1, mc) + u.move - 1) / u.move;
                    }
                    minTurn = std::min(minTurn, ft);
                    // Both minTurn and minCost are monotonically non-increasing across friendlies.
                    if (minTurn < floorTurn || (minTurn == floorTurn && minCost < floorCost))
                    {
                        pruned = true;
                        return std::tuple{minTurn, minCost, 0};
                    }
                }
                const int stars = m_local.defense ? m_local.defense(le.info->ki, q).stars : 0;
                return std::tuple{minTurn, minCost, stars};
            };
            TilePoint bestQ = le.pos;
            bool dummyPruned = false;
            auto bestSc = escapeScore(le.pos, std::numeric_limits<int>::min(), std::numeric_limits<int>::min(), dummyPruned);
            for (const TilePoint q : m_escapeTilesScratch)
            {
                bool pruned = false;
                const auto sc = escapeScore(q, std::get<0>(bestSc), std::get<1>(bestSc), pruned);
                if (!pruned && sc > bestSc)
                {
                    bestSc = sc;
                    bestQ = q;
                }
            }
            if (bestQ != le.pos)
            {
                if (le.pos.x >= 0 && le.pos.x < m_local.width && le.pos.y >= 0 && le.pos.y < m_local.height)
                {
                    const std::size_t oldIdx = static_cast<std::size_t>(le.pos.y) * static_cast<std::size_t>(m_local.width) +
                        static_cast<std::size_t>(le.pos.x);
                    if (m_occEnemyKi[oldIdx] == le.info->ki)
                        m_occEnemyGen[oldIdx] = 0;
                }
                le.pos = bestQ;
                le.fleeingE0 = std::get<0>(bestSc) > 1;
                if (bestQ.x >= 0 && bestQ.x < m_local.width && bestQ.y >= 0 && bestQ.y < m_local.height)
                {
                    const std::size_t newIdx = static_cast<std::size_t>(bestQ.y) * static_cast<std::size_t>(m_local.width) +
                        static_cast<std::size_t>(bestQ.x);
                    m_occEnemyGen[newIdx] = occEnemyGen;
                    m_occEnemyKi[newIdx] = le.info->ki;
                }
                if (m_fast)
                    m_fast->enemyReach.clear();
            }
        }

        struct ForwardArc
        {
            std::size_t ei{0};
            std::int32_t fireTurn{0};
            MilliFunds netTradeMilli{0};
            double share{0.0};
        };
        struct ResponderPlan
        {
            std::size_t fi{0};
            std::int32_t minFireTurn{0};
            std::vector<ForwardArc> arcs;
        };
        std::vector<ResponderPlan> plans;
        plans.reserve(friendlyCount);
        for (std::size_t fi = 0; fi < friendlyCount; ++fi)
        {
            const auto & u = friendlies[fi];
            ResponderPlan rp;
            rp.fi = fi;
            rp.minFireTurn = horizon;

            for (std::size_t ei = 0; ei < enemyCount; ++ei)
            {
                const auto & le = liveEnemies[ei];
                const double baseDmg = maxWeaponBaseDamage(u.ki, le.info->ki, u.minR);
                if (baseDmg <= 0.0)
                    continue;
                int moveCost = attackRingPathCost(u.ki, u.pos, le.pos, u.minR, u.maxR);
                if (le.firedInE0 && !le.usedInF1Ret)
                    moveCost = std::max(moveCost, attackRingPathCost(u.ki, u.pos, le.info->initialTile, u.minR, u.maxR));
                if (moveCost >= UNIFIED_UNREACHABLE_COST)
                    continue;
                std::int32_t fireTurn = horizon;
                if (u.minR <= 1)
                {
                    if (u.move <= 0 && moveCost > 0)
                        continue;
                    const std::int32_t mt = u.move > 0 ? (moveCost + u.move - 1) / u.move : 0;
                    fireTurn = std::max<std::int32_t>(u.readyTurn, u.readyTurn - 1 + mt);
                    if (le.fleeingE0 && fireTurn > u.readyTurn)
                    {
                        if (u.move <= le.info->move)
                            continue;
                        const std::int32_t remCost = std::max<std::int32_t>(1, moveCost - u.move);
                        const std::int32_t closeRate = u.move - le.info->move;
                        fireTurn = u.readyTurn + (remCost + closeRate - 1) / closeRate;
                    }
                }
                else
                {
                    const bool jammedInBlindSpot = std::ranges::any_of(liveEnemies, [&](const LiveEnemyState & otherLe) {
                        if (manhattanDistance(u.pos, otherLe.pos) >= u.minR)
                            return false;
                        const int eMove = otherLe.info->canMoveAndFire ? otherLe.info->move : 0;
                        return canEnemyReachAttackOccupied(otherLe.info->ki, otherLe.pos, u.pos, eMove,
                                                           otherLe.info->minRange, otherLe.info->maxRange,
                                                           occFriendlyGen) &&
                            maxWeaponBaseDamage(otherLe.info->ki, u.ki, otherLe.info->minRange) > 0.0;
                    });
                    if (!jammedInBlindSpot && u.readyTurn == 1 && moveCost == 0 &&
                        manhattanDistance(u.pos, le.pos) >= u.minR &&
                        manhattanDistance(u.pos, le.pos) <= u.maxR)
                    {
                        fireTurn = 1;
                    }
                    else if (u.move > 0)
                    {
                        if (le.fleeingE0 && u.move <= le.info->move)
                            continue;
                        const std::int32_t mt = (std::max(1, moveCost) + u.move - 1) / u.move;
                        fireTurn = u.readyTurn + mt;
                    }
                }
                if (fireTurn >= horizon)
                    continue;

                const TerrainDefenseRule eDef = m_local.defense ? m_local.defense(le.info->ki, le.pos) : TerrainDefenseRule{};
                const double rawHit = terrainReducedDamage(eDef, baseDmg, u.hp, ceilHpSteps(le.hp));
                const double hit = engineShotDamage(rawHit);
                if (hit <= 0.0)
                    continue;
                const double eAfter = engineHpAfterReply(le.hp, hit);
                const MilliFunds eLoss = combatCapitalLoss(le.info->cost, le.hp, eAfter);
                MilliFunds uCounter = 0;
                if (u.minR == 1 && le.info->minRange == 1 && eAfter > 0.0)
                {
                    const double retDmg = maxWeaponBaseDamage(le.info->ki, u.ki, 1);
                    if (retDmg > 0.0)
                    {
                        const std::int32_t fireMt = u.move > 0 ? (moveCost + u.move - 1) / u.move : 0;
                        const TerrainDefenseRule fireDef = bestMeleeFiringDefense(
                            u.ki, u.pos, le.pos, u.move, std::max<std::int32_t>(1, fireMt) * std::max(1, u.move));
                        const double rawC = sameActionCounterDamage(fireDef, retDmg, u.hp, rawHit, ceilHpSteps(le.hp));
                        uCounter = combatCapitalLoss(u.cost, u.hp, engineHpAfterReply(u.hp, engineShotDamage(rawC)));
                    }
                }
                else if (u.minR > 1 && fireTurn == 1)
                {
                    // Stationary F1 indirect fire draws unscreened E1 retaliation if no friendly unit blocks or covers u.pos.
                    const TerrainDefenseRule uDef = m_local.defense ? m_local.defense(u.ki, u.pos) : TerrainDefenseRule{};
                    for (const auto & otherLe : liveEnemies)
                    {
                        const double otherHp = (&otherLe == &le) ? eAfter : otherLe.hp;
                        if (otherHp <= 0.0 || !otherLe.info->canMoveAndFire)
                            continue;
                        const double retBase = maxWeaponBaseDamage(otherLe.info->ki, u.ki, otherLe.info->minRange);
                        if (retBase <= 0.0)
                            continue;
                        const int eMove = std::max(0, otherLe.info->move);
                        if (eMove <= 0 ||
                            !canEnemyReachAttackOccupied(otherLe.info->ki, otherLe.pos, u.pos, eMove,
                                                         otherLe.info->minRange, otherLe.info->maxRange,
                                                         occFriendlyGen))
                            continue;
                        const MilliFunds eHitLoss = combatCapitalLoss(
                            u.cost, u.hp,
                            engineHpAfterReply(u.hp, engineShotDamage(
                                terrainReducedDamage(uDef, retBase, otherHp, ceilHpSteps(u.hp)))));
                        const bool coveredByFriendly = std::ranges::any_of(friendlies, [&](const FriendlyForwardUnit & fc) {
                            if (fc.ki == u.ki || !fc.directCombat)
                                return false;
                            const double cBase = maxWeaponBaseDamage(fc.ki, otherLe.info->ki, 1);
                            if (cBase <= 0.0 ||
                                !canFriendlyReachAttackOccupied(fc.ki, fc.pos, u.pos, fc.move, 1, 1, 0))
                                return false;
                            const double fcHit = engineShotDamage(
                                terrainReducedDamage(TerrainDefenseRule{}, cBase, fc.hp, ceilHpSteps(otherHp)));
                            return combatCapitalLoss(otherLe.info->cost, otherHp, engineHpAfterReply(otherHp, fcHit)) >= eHitLoss;
                        });
                        if (!coveredByFriendly)
                            uCounter = std::max(uCounter, eHitLoss);
                    }
                }
                const MilliFunds netTradeMilli = eLoss - uCounter;
                if (netTradeMilli <= 0)
                    continue;
                const double share = le.liveCap > 0
                    ? std::min(1.0, static_cast<double>(netTradeMilli) / static_cast<double>(le.liveCap))
                    : 1.0;
                rp.minFireTurn = std::min(rp.minFireTurn, fireTurn);
                rp.arcs.push_back({ei, fireTurn, netTradeMilli, share});
            }
            if (!rp.arcs.empty())
                plans.push_back(std::move(rp));
        }

        // Allocate specialized responders (fewest positive-trade targets) then earliest arrival first.
        std::stable_sort(plans.begin(), plans.end(), [](const ResponderPlan & a, const ResponderPlan & b) {
            return std::tuple{a.arcs.size(), a.minFireTurn, a.fi} <
                std::tuple{b.arcs.size(), b.minFireTurn, b.fi};
        });

        std::vector<MilliFunds> remCap(enemyCount, 0);
        for (std::size_t ei = 0; ei < enemyCount; ++ei)
            remCap[ei] = liveEnemies[ei].liveCap;

        MilliFunds dOurs = 0;
        for (auto & rp : plans)
        {
            auto & ranked = rp.arcs;
            const auto arcGain = [&](const ForwardArc & arc) -> MilliFunds {
                if (remCap[arc.ei] <= 0)
                    return 0;
                const MilliFunds grantedTrade = std::min(arc.netTradeMilli, remCap[arc.ei]);
                return (grantedTrade * (horizon - arc.fireTurn)) / horizon;
            };
            std::stable_sort(ranked.begin(), ranked.end(), [&](const ForwardArc & a, const ForwardArc & b) {
                const MilliFunds sa = arcGain(a);
                const MilliFunds sb = arcGain(b);
                return sa != sb ? sa > sb : a.fireTurn < b.fireTurn;
            });
            double actionBudget = 1.0;
            for (const auto & arc : ranked)
            {
                if (actionBudget <= 0.0)
                    break;
                if (remCap[arc.ei] <= 0)
                    continue;
                const double usableShare = std::min(actionBudget, arc.share);
                const MilliFunds maxSpend = arc.share > 0.0
                    ? static_cast<MilliFunds>(std::llround(static_cast<double>(arc.netTradeMilli) * (usableShare / arc.share)))
                    : arc.netTradeMilli;
                const MilliFunds grantedTrade = std::min({maxSpend, arc.netTradeMilli, remCap[arc.ei]});
                if (grantedTrade <= 0)
                    continue;
                const MilliFunds timedTrade = (grantedTrade * (horizon - arc.fireTurn)) / horizon;
                RelativeStockDetail::checkedAdd(dOurs, timedTrade);
                const double consumedShare = arc.netTradeMilli > 0
                    ? arc.share * (static_cast<double>(grantedTrade) / static_cast<double>(arc.netTradeMilli))
                    : 1.0;
                actionBudget -= std::max(consumedShare, 0.0);
                remCap[arc.ei] -= grantedTrade;
            }
        }

        // Conserved D_enemy on asymmetric uncounterable predators (e.g. Bombers vs ground, Subs vs surface).
        std::vector<MilliFunds> remFriendlyCap(friendlyCount, 0);
        for (std::size_t fi = 0; fi < friendlyCount; ++fi)
            remFriendlyCap[fi] = combatCapitalLoss(friendlies[fi].cost, friendlies[fi].hp, 0.0);
        MilliFunds dEnemy = 0;
        for (std::size_t ei = 0; ei < enemyCount; ++ei)
        {
            const auto & le = liveEnemies[ei];
            if (remCap[ei] <= 0 || le.info->cost <= 0)
                continue;
            const double eRemHp = le.liveCap > 0
                ? le.hp * (static_cast<double>(remCap[ei]) / static_cast<double>(le.liveCap))
                : le.hp;
            if (eRemHp <= 0.0)
                continue;
            std::vector<ForwardArc> eArcs;
            for (std::size_t fi = 0; fi < friendlyCount; ++fi)
            {
                const auto & u = friendlies[fi];
                if (remFriendlyCap[fi] <= 0)
                    continue;
                if (maxWeaponBaseDamage(u.ki, le.info->ki, u.minR) > 0.0)
                    continue;
                const double eBase = maxWeaponBaseDamage(le.info->ki, u.ki, le.info->minRange);
                if (eBase <= 0.0)
                    continue;
                const int eMc = attackRingPathCost(
                    le.info->ki, le.pos, u.pos, le.info->minRange, le.info->maxRange);
                if (eMc >= UNIFIED_UNREACHABLE_COST)
                    continue;
                std::int32_t eFt = horizon;
                if (le.info->canMoveAndFire)
                {
                    if (le.info->move <= 0 && eMc > 0)
                        continue;
                    const std::int32_t mt = le.info->move > 0 ? (eMc + le.info->move - 1) / le.info->move : 0;
                    eFt = std::max<std::int32_t>(1, mt);
                }
                else if (eMc == 0 &&
                         manhattanDistance(le.pos, u.pos) >= le.info->minRange &&
                         manhattanDistance(le.pos, u.pos) <= le.info->maxRange)
                {
                    eFt = 1;
                }
                else if (le.info->move > 0)
                {
                    eFt = 1 + (std::max(1, eMc) + le.info->move - 1) / le.info->move;
                }
                if (eFt >= horizon)
                    continue;
                // Only E1 imminent strikes depend on u.pos terrain and 1-turn AA cover; distant predators use neutral cover and eFt-turn intercept.
                const bool imminentStrike = (eFt <= 1);
                const TerrainDefenseRule uDef = (imminentStrike && m_local.defense)
                    ? m_local.defense(u.ki, u.pos) : TerrainDefenseRule{};
                const double rawHit = terrainReducedDamage(uDef, eBase, eRemHp, ceilHpSteps(u.hp));
                const double hit = engineShotDamage(rawHit);
                if (hit <= 0.0)
                    continue;
                const MilliFunds uLoss = combatCapitalLoss(u.cost, u.hp, engineHpAfterReply(u.hp, hit));
                if (uLoss <= 0)
                    continue;
                MilliFunds maxCoverRetaliation = 0;
                for (const auto & fc : friendlies)
                {
                    if (fc.ki == u.ki)
                        continue;
                    const double cBase = maxWeaponBaseDamage(fc.ki, le.info->ki, fc.minR);
                    if (cBase <= 0.0)
                        continue;
                    const int cMc = attackRingPathCost(fc.ki, fc.pos, u.pos, fc.minR, fc.maxR);
                    const bool coversBeforeStrike = fc.minR <= 1
                        ? (cMc == 0 || (fc.move > 0 && cMc <= fc.move * eFt))
                        : (cMc == 0 ? manhattanDistance(fc.pos, u.pos) <= fc.maxR
                                    : (fc.move > 0 && eFt > 1 && cMc <= fc.move * (eFt - 1)));
                    if (!coversBeforeStrike)
                        continue;
                    const double fcHit = engineShotDamage(
                        terrainReducedDamage(TerrainDefenseRule{}, cBase, fc.hp, ceilHpSteps(eRemHp)));
                    const MilliFunds retLoss = combatCapitalLoss(le.info->cost, eRemHp, engineHpAfterReply(eRemHp, fcHit));
                    maxCoverRetaliation = std::max(maxCoverRetaliation, retLoss);
                }
                const MilliFunds netEnemyTrade = uLoss - maxCoverRetaliation;
                if (netEnemyTrade <= 0)
                    continue;
                const std::int32_t effFt = imminentStrike ? 1 : 2;
                const double share = remFriendlyCap[fi] > 0
                    ? std::min(1.0, static_cast<double>(netEnemyTrade) / static_cast<double>(remFriendlyCap[fi]))
                    : 1.0;
                eArcs.push_back({fi, effFt, netEnemyTrade, share});
            }
            std::stable_sort(eArcs.begin(), eArcs.end(), [&](const ForwardArc & a, const ForwardArc & b) {
                const MilliFunds ga = (std::min(a.netTradeMilli, remFriendlyCap[a.ei]) * (horizon - a.fireTurn)) / horizon;
                const MilliFunds gb = (std::min(b.netTradeMilli, remFriendlyCap[b.ei]) * (horizon - b.fireTurn)) / horizon;
                return ga != gb ? ga > gb : a.fireTurn < b.fireTurn;
            });
            double eBudget = 1.0;
            for (const auto & arc : eArcs)
            {
                if (eBudget <= 0.0)
                    break;
                if (remFriendlyCap[arc.ei] <= 0)
                    continue;
                const double useS = std::min(eBudget, arc.share);
                const MilliFunds maxSp = arc.share > 0.0
                    ? static_cast<MilliFunds>(std::llround(static_cast<double>(arc.netTradeMilli) * (useS / arc.share)))
                    : arc.netTradeMilli;
                const MilliFunds granted = std::min({maxSp, arc.netTradeMilli, remFriendlyCap[arc.ei]});
                if (granted <= 0)
                    continue;
                RelativeStockDetail::checkedAdd(dEnemy, (granted * (horizon - arc.fireTurn)) / horizon);
                const double usedShare = arc.netTradeMilli > 0
                    ? arc.share * (static_cast<double>(granted) / static_cast<double>(arc.netTradeMilli))
                    : 1.0;
                eBudget -= std::max(usedShare, 0.0);
                remFriendlyCap[arc.ei] -= granted;
            }
        }
        return dOurs - dEnemy;
    }

    bool UnifiedTeamEvaluator::isOnlyHitByDeterred(const ReplyPassEvaluation & eval, int targetKi,
                             std::span<const int> deterredShooters, double & preDeterredHpOut) const
    {
        if (deterredShooters.empty())
            return false;
        bool anyShot = false;
        double earliestBefore = -1.0;
        for (const ReplyPassShot & s : eval.shots)
        {
            if (s.targetKnowledge != targetKi)
                continue;
            if (std::ranges::find(deterredShooters, s.shooterIndex) == deterredShooters.end())
                return false;
            if (!anyShot)
                earliestBefore = s.realBefore;
            anyShot = true;
        }
        if (anyShot && earliestBefore > 0.0)
        {
            preDeterredHpOut = earliestBefore;
            return true;
        }
        return false;
    }

    std::vector<std::uint8_t> UnifiedTeamEvaluator::computeBlockedStockNodes(
        const ReplyPassEvaluation & eval,
        std::span<const int> vacateKills,
        std::span<const int> deterredShooters) const
    {
        const std::size_t nodeCount = m_stockContext.nodeCount();
        std::vector<std::uint8_t> blocked(nodeCount, 0);
        if (nodeCount == 0 || m_sampleCapperKi == NO_UNIT || (!m_local.weapon && !m_local.weaponSlots))
            return blocked;
        const std::int32_t capPts = m_stockField.capturePointsToCapture();
        struct ActiveNodeCapper
        {
            int capperKi{NO_UNIT};
            int carried{0};
            double realHp{static_cast<double>(UNIT_HP_STEPS)};
            int steps{UNIT_HP_STEPS};
            int capRate{UNIT_HP_STEPS};
            int origHp{UNIT_HP_STEPS};
        };
        struct SurvivingCapperRef
        {
            int ki{NO_UNIT};
            TilePoint pos{INVALID_TILE};
            int move{1};
        };
        std::vector<bool> finishingNow(nodeCount, false);
        std::vector<std::optional<ActiveNodeCapper>> activeCapper(nodeCount);
        std::vector<CaptureCoverActor> coverUnits;
        std::vector<SurvivingCapperRef> survivingCappers;
        std::vector<std::uint8_t> seenCapperKi(m_local.units.size(), 0);
        for (const auto & actor : eval.actors)
        {
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const int ki = member.knowledgeUnitIndex;
            if (ki >= 0 && ki < static_cast<int>(seenCapperKi.size()))
                seenCapperKi[static_cast<std::size_t>(ki)] = 1;
            if (std::ranges::find(vacateKills, ki) != vacateKills.end()) continue;
            double deterredHp = 0.0;
            const bool restored = isOnlyHitByDeterred(eval, ki, deterredShooters, deterredHp);
            const int effSteps = restored ? actor.stepsAfterOwnActions : actor.stepsAfterReply;
            if (effSteps <= 0) continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
            if (member.stockRow == NO_STOCK_ROW)
            {
                if (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
                {
                    const auto & knownF = m_knowledge.units()[static_cast<std::size_t>(ki)];
                    const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
                    const int minR = std::max(1, knownF.minRange);
                    const int maxR = std::max(1, knownF.maxRange);
                    const int rawM = std::max(0, fp.movement);
                    const int moveB = minR > 1 ? 0 : rawM;
                    double hpF = restored ? deterredHp : static_cast<double>(effSteps);
                    if (!restored)
                    {
                        if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                            hpF = it->second;
                    }
                    if (hpF > 0.0)
                        coverUnits.push_back({ki, cand.destination, hpF, moveB, minR, maxR, fp.replacementCost, rawM});
                }
                continue;
            }
            if (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
            {
                survivingCappers.push_back({ki, cand.destination,
                                            std::max(1, m_local.units[static_cast<std::size_t>(ki)].movement)});
            }
            if (!cand.capture.has_value()) continue;
            const bool hasKnown = ki >= 0 && ki < static_cast<int>(m_knowledge.units().size());
            const int capRate = hasKnown ? m_knowledge.units()[static_cast<std::size_t>(ki)].captureRate : UNIT_HP_STEPS;
            const int origPts = hasKnown ? m_knowledge.units()[static_cast<std::size_t>(ki)].capturePoints : 0;
            const int origHp = hasKnown ? m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded : UNIT_HP_STEPS;
            const int carried = (cand.destination == cand.origin ? origPts : 0) + capRate;
            double realHp = restored ? deterredHp : static_cast<double>(effSteps);
            if (!restored)
            {
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    realHp = it->second;
            }
            for (std::size_t n = 0; n < nodeCount; ++n)
            {
                if (m_stockContext.nodeTile(n) == cand.destination)
                {
                    if (carried >= capPts)
                        finishingNow[n] = true;
                    else
                        activeCapper[n] = ActiveNodeCapper{ki, carried, realHp, effSteps, capRate, origHp};
                    break;
                }
            }
        }
        for (const auto & obs : m_local.observedDefenders)
        {
            const int ki = obs.knowledgeIndex;
            if (ki < 0 || ki >= static_cast<int>(m_local.units.size()) ||
                seenCapperKi[static_cast<std::size_t>(ki)] != 0 ||
                std::ranges::find(vacateKills, ki) != vacateKills.end())
                continue;
            double deterredHp = 0.0;
            double hpObs = obs.facts ? obs.facts->actorRealHp : UNIFIED_HP_STEPS_DOUBLE;
            if (isOnlyHitByDeterred(eval, ki, deterredShooters, deterredHp))
                hpObs = deterredHp;
            else if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                hpObs = it->second;
            if (hpObs <= 0.0)
                continue;
            if (obs.stockRow == NO_STOCK_ROW)
            {
                if (ki < static_cast<int>(m_knowledge.units().size()) &&
                    m_knowledge.units()[static_cast<std::size_t>(ki)].hasWeapons)
                {
                    const auto & knownObs = m_knowledge.units()[static_cast<std::size_t>(ki)];
                    const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
                    const int minR = std::max(1, knownObs.minRange);
                    const int maxR = std::max(1, knownObs.maxRange);
                    const int rawM = std::max(0, fp.movement);
                    const int moveB = minR > 1 ? 0 : rawM;
                    coverUnits.push_back({ki, obs.tile, hpObs, moveB, minR, maxR, fp.replacementCost, rawM});
                }
                continue;
            }
            survivingCappers.push_back({ki, obs.tile, std::max(1, m_local.units[static_cast<std::size_t>(ki)].movement)});
        }
        std::vector<CaptureCoverActor> defenders;
        std::vector<std::int32_t> killedInF0;
        for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
        {
            const auto & ep = m_local.units[static_cast<std::size_t>(e)];
            if (!ep.present || !ep.enemy) continue;
            const bool hasKnownE = e >= 0 && e < static_cast<int>(m_knowledge.units().size());
            const double initialHp = ep.realHp >= 0.0
                ? ep.realHp
                : static_cast<double>(hasKnownE ? m_knowledge.units()[static_cast<std::size_t>(e)].hpRounded : UNIT_HP_STEPS);
            double hpOwn = initialHp;
            if (const auto itOwn = eval.enemyAfterOwn.find(e); itOwn != eval.enemyAfterOwn.end())
                hpOwn = static_cast<double>(itOwn->second);
            if (hpOwn <= 0.0)
            {
                killedInF0.push_back(e);
                continue;
            }
            double hpAfterE0 = hpOwn;
            if (std::ranges::find(deterredShooters, e) == deterredShooters.end())
            {
                if (const auto itReply = eval.enemyReplyHp.find(e); itReply != eval.enemyReplyHp.end())
                    hpAfterE0 = std::min(hpAfterE0, static_cast<double>(itReply->second));
            }
            if (hpAfterE0 <= 0.0)
                continue;
            const TilePoint ePos = (e < static_cast<int>(eval.combatLocations.size()) &&
                                    eval.combatLocations[static_cast<std::size_t>(e)] != INVALID_TILE)
                ? eval.combatLocations[static_cast<std::size_t>(e)]
                : ep.tile;
            const int minR = hasKnownE ? std::max(1, m_knowledge.units()[static_cast<std::size_t>(e)].minRange) : 1;
            const int maxR = hasKnownE ? std::max(1, m_knowledge.units()[static_cast<std::size_t>(e)].maxRange) : 1;
            const int moveB = minR > 1 ? 0 : std::max(0, ep.movement);
            defenders.push_back({e, ePos, hpAfterE0, moveB, minR, maxR, ep.replacementCost});
        }
        const std::uint32_t occEnemyGen = nextOccEnemyGen();
        for (const auto & d : defenders)
        {
            if (d.pos.x >= 0 && d.pos.x < m_local.width && d.pos.y >= 0 && d.pos.y < m_local.height)
            {
                const std::size_t idx = static_cast<std::size_t>(d.pos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(d.pos.x);
                m_occEnemyGen[idx] = occEnemyGen;
                m_occEnemyKi[idx] = d.ki;
            }
        }
        const std::uint32_t occFriendlyGen = nextOccFriendlyGen();
        std::vector<std::uint8_t> friendlyActorKi(m_local.units.size(), 0);
        for (const auto & actor : eval.actors)
        {
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const int ki = member.knowledgeUnitIndex;
            if (ki >= 0 && ki < static_cast<int>(friendlyActorKi.size()))
                friendlyActorKi[static_cast<std::size_t>(ki)] = 1;
            if (actor.stepsAfterReply <= 0 || std::ranges::find(vacateKills, ki) != vacateKills.end())
                continue;
            const TilePoint fPos = member.candidates[static_cast<std::size_t>(actor.candidateIndex)].destination;
            const bool exposedIndirectBlocker =
                ki >= 0 && ki < static_cast<int>(m_knowledge.units().size()) &&
                m_knowledge.units()[static_cast<std::size_t>(ki)].minRange > 1 &&
                std::ranges::any_of(defenders, [&](const CaptureCoverActor & d) {
                    return canEnemyReachAttackOccupied(d.ki, d.pos, fPos, d.moveBudget, d.strikeRange, d.maxRange, 0) &&
                        maxWeaponBaseDamage(d.ki, ki, d.strikeRange) > 0.0;
                });
            if (!exposedIndirectBlocker &&
                fPos.x >= 0 && fPos.x < m_local.width && fPos.y >= 0 && fPos.y < m_local.height)
            {
                const std::size_t idx = static_cast<std::size_t>(fPos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(fPos.x);
                m_occFriendlyGen[idx] = occFriendlyGen;
                m_occFriendlyKi[idx] = ki;
            }
        }
        for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
        {
            if (friendlyActorKi[static_cast<std::size_t>(ki)] != 0) continue;
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            if (!fp.present || fp.enemy || fp.tile == INVALID_TILE ||
                std::ranges::find(vacateKills, ki) != vacateKills.end())
                continue;
            if (fp.tile.x >= 0 && fp.tile.x < m_local.width && fp.tile.y >= 0 && fp.tile.y < m_local.height)
            {
                const std::size_t idx = static_cast<std::size_t>(fp.tile.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(fp.tile.x);
                m_occFriendlyGen[idx] = occFriendlyGen;
                m_occFriendlyKi[idx] = ki;
            }
        }
        std::erase_if(coverUnits, [&](const CaptureCoverActor & fc) {
            if (fc.strikeRange <= 1)
                return false;
            return std::ranges::any_of(defenders, [&](const CaptureCoverActor & d) {
                return manhattanDistance(fc.pos, d.pos) < fc.strikeRange &&
                    canEnemyReachAttackOccupied(d.ki, d.pos, fc.pos, d.moveBudget, d.strikeRange, d.maxRange,
                                                occFriendlyGen) &&
                    maxWeaponBaseDamage(d.ki, fc.ki, d.strikeRange) > 0.0;
            });
        });
        struct UncoveredDefender
        {
            const CaptureCoverActor * d{nullptr};
            double effHp{0.0};
            double baseDmg{0.0};
            std::uint8_t adjMask{0};
        };
        std::vector<UncoveredDefender> uncovered;
        std::vector<std::uint8_t> usedCover(coverUnits.size(), 0);
        for (std::size_t n = 0; n < nodeCount; ++n)
        {
            if (finishingNow[n]) continue;
            const TilePoint tile = m_stockContext.nodeTile(n);
            if (tile == INVALID_TILE) continue;
            const bool nearEnemyFactory = (m_fast && n < m_fast->nodeNearEnemyFactory.size())
                ? (m_fast->nodeNearEnemyFactory[n] != 0)
                : std::ranges::any_of(m_enemyFactories, [&](TilePoint f) {
                      return enemyFactoryThreatensTile(f, tile);
                  });
            const OwnerSign owner = m_stockContext.nodeOwner(n);
            int capperArriveTurns = 1;
            if (!activeCapper[n])
            {
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
                int minArrive = UNIFIED_UNREACHABLE_COST;
                for (const auto & sc : survivingCappers)
                {
                    const int pathCost = gridMoveCostForUnit(sc.ki, sc.pos, tile);
                    if (pathCost < UNIFIED_UNREACHABLE_COST)
                        minArrive = std::min(minArrive, std::max(1, (pathCost + sc.move - 1) / sc.move));
                }
                if (minArrive >= UNIFIED_UNREACHABLE_COST)
                {
                    blocked[n] = 1;
                    continue;
                }
                capperArriveTurns = minArrive;
            }
            const int capKi = activeCapper[n] && activeCapper[n]->capperKi >= 0
                ? activeCapper[n]->capperKi : m_sampleCapperKi;
            const int baseCarried = activeCapper[n] ? activeCapper[n]->carried : 0;
            const double baseHp = activeCapper[n] ? activeCapper[n]->realHp : static_cast<double>(UNIT_HP_STEPS);
            const int baseSteps = activeCapper[n] ? activeCapper[n]->steps : UNIT_HP_STEPS;
            const int capOrigRate = activeCapper[n] ? activeCapper[n]->capRate : UNIT_HP_STEPS;
            const int capOrigHp = activeCapper[n] ? activeCapper[n]->origHp : UNIT_HP_STEPS;
            const TerrainDefenseRule defRule = m_local.defense
                ? m_local.defense(capKi, tile) : TerrainDefenseRule{};

            // Evaluate conserved E1/E2 responder pressure on unscreened captures with distinct melee slots.
            uncovered.clear();
            std::fill(usedCover.begin(), usedCover.end(), 0);
            for (const auto & d : defenders)
            {
                const double baseDmg = maxWeaponBaseDamage(d.ki, capKi, d.strikeRange);
                if (baseDmg <= 0.0) continue;
                std::uint8_t adjMask = 0;
                if (d.strikeRange == 1)
                {
                    for (std::uint8_t dirIdx = 0; dirIdx < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++dirIdx)
                    {
                        const StepVector step = entryStep(ENTRY_DIRECTIONS[dirIdx]);
                        const TilePoint adj{tile.x + step.dx, tile.y + step.dy};
                        if (canEnemyReachFiringSquare(d.ki, d.pos, adj, d.moveBudget, occFriendlyGen))
                            adjMask |= static_cast<std::uint8_t>(1u << dirIdx);
                    }
                    if (adjMask == 0)
                        continue;
                }
                else if (!canEnemyReachAttackOccupied(d.ki, d.pos, tile, d.moveBudget, d.strikeRange, d.maxRange,
                                                      occFriendlyGen))
                {
                    continue;
                }
                double effHp = d.hp;
                bool eliminatedByCover = false;
                for (std::size_t ci = 0; ci < coverUnits.size(); ++ci)
                {
                    if (usedCover[ci] != 0)
                        continue;
                    CaptureCoverActor effFc = coverUnits[ci];
                    if (effFc.ki == capKi)
                        continue;
                    if (!activeCapper[n])
                    {
                        effFc.moveBudget = effFc.strikeRange > 1
                            ? (capperArriveTurns > 1 ? (capperArriveTurns - 1) * effFc.rawMove : 0)
                            : std::max(1, capperArriveTurns) * effFc.rawMove;
                    }
                    const auto afterD = defenderHpAfterCover(effFc, d, tile, occEnemyGen, occFriendlyGen);
                    if (!afterD.has_value())
                        continue;
                    usedCover[ci] = 1;
                    if (!activeCapper[n] || *afterD <= 0.0)
                    {
                        eliminatedByCover = true;
                    }
                    else
                    {
                        effHp = *afterD;
                    }
                    break;
                }
                if (eliminatedByCover) continue;
                uncovered.push_back({&d, effHp, baseDmg, adjMask});
            }
            std::stable_sort(uncovered.begin(), uncovered.end(), [](const UncoveredDefender & a, const UncoveredDefender & b) {
                const double sa = a.baseDmg * a.effHp;
                const double sb = b.baseDmg * b.effHp;
                if (sa != sb) return sa > sb;
                return std::popcount(a.adjMask) < std::popcount(b.adjMask);
            });
            // An active F0 capper surviving E0 takes its F1 capture step before E1 happens.
            int carriedBeforeE1 = baseCarried;
            if (activeCapper[n])
            {
                const int rateAtF1 = replyCaptureRate(
                    capOrigRate, std::max(0, capOrigHp - baseSteps), false);
                carriedBeforeE1 += rateAtF1;
                if (carriedBeforeE1 >= capPts) continue;
            }
            double remHpAfterE1 = baseHp;
            int remStepsAfterE1 = baseSteps;
            bool anyUncoveredHit = false;
            double strongestFollowupBase = 0.0;
            double strongestFollowupHp = 0.0;
            std::uint8_t usedAdjMask = 0;
            for (const auto & item : uncovered)
            {
                const auto & d = *item.d;
                if (d.strikeRange == 1)
                {
                    const std::uint8_t freeMask = static_cast<std::uint8_t>(item.adjMask & ~usedAdjMask);
                    if (freeMask == 0)
                        continue;
                    for (std::uint8_t dirIdx = 0; dirIdx < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++dirIdx)
                    {
                        if ((freeMask & static_cast<std::uint8_t>(1u << dirIdx)) != 0)
                        {
                            usedAdjMask |= static_cast<std::uint8_t>(1u << dirIdx);
                            break;
                        }
                    }
                }
                anyUncoveredHit = true;
                const double e1Hit = engineShotDamage(terrainReducedDamage(defRule, item.baseDmg, item.effHp, remStepsAfterE1));
                remHpAfterE1 = engineHpAfterReply(remHpAfterE1, e1Hit);
                remStepsAfterE1 = ceilHpSteps(remHpAfterE1);
                if (item.baseDmg * item.effHp > strongestFollowupBase * strongestFollowupHp)
                {
                    strongestFollowupBase = item.baseDmg;
                    strongestFollowupHp = item.effHp;
                }
                if (remStepsAfterE1 <= 0) break;
            }
            if (!anyUncoveredHit) continue;
            if (remStepsAfterE1 <= 0)
            {
                blocked[n] = 1;
                continue;
            }
            const int rateAfterE1 = replyCaptureRate(
                capOrigRate, std::max(0, capOrigHp - remStepsAfterE1), false);
            if (carriedBeforeE1 + rateAfterE1 >= capPts) continue;
            const double e2Hit = engineShotDamage(
                terrainReducedDamage(defRule, strongestFollowupBase, strongestFollowupHp, remStepsAfterE1));
            const double hp2 = engineHpAfterReply(remHpAfterE1, e2Hit);
            const int s2 = ceilHpSteps(hp2);
            const int rateAfterE2 = replyCaptureRate(
                capOrigRate, std::max(0, capOrigHp - s2), s2 <= 0);
            if (s2 <= 0 || carriedBeforeE1 + rateAfterE1 + rateAfterE2 < capPts)
                blocked[n] = 1;
        }
        return blocked;
    }

    MilliFunds UnifiedTeamEvaluator::cachedPostReplyStock(const ReplyPassEvaluation & eval,
                                    std::span<const int> vacateKills,
                                    std::span<const int> deterredShooters)
    {
        ContinuationKey key = eval.afterReply;
        if (!deterredShooters.empty())
        {
            for (const auto & actor : eval.actors)
            {
                if (actor.stockRow == NO_STOCK_ROW)
                    continue;
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                const int ki = member.knowledgeUnitIndex;
                if (std::ranges::find(vacateKills, ki) != vacateKills.end())
                    continue;
                double deterredHp = 0.0;
                if (!isOnlyHitByDeterred(eval, ki, deterredShooters, deterredHp))
                    continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                const int lostOwn = std::max(0, actor.dayStartSteps - actor.stepsAfterOwnActions);
                const bool deadOwn = actor.stepsAfterOwnActions <= 0;
                for (CanonicalPlanActionKey & act : key)
                {
                    if (act.row() == actor.stockRow)
                    {
                        if (const auto updated = CanonicalPlanActionKey::checkedFromAction(
                                actor.stockRow, cand.destination.x, cand.destination.y,
                                cand.capture.has_value() && !deadOwn, lostOwn, deadOwn);
                            updated.has_value())
                        {
                            act = *updated;
                        }
                    }
                }
            }
        }
        for (const int killedKi : vacateKills)
        {
            int row = NO_STOCK_ROW;
            for (const auto & member : m_problem.actors())
            {
                if (member.knowledgeUnitIndex == killedKi)
                {
                    row = member.stockRow;
                    break;
                }
            }
            if (row == NO_STOCK_ROW)
                continue;
            for (CanonicalPlanActionKey & act : key)
            {
                if (act.row() == row)
                {
                    if (const auto updated = CanonicalPlanActionKey::checkedFromAction(
                            row, act.x(), act.y(), false, act.stepsLost(), true);
                        updated.has_value())
                    {
                        act = *updated;
                    }
                }
            }
        }
        const std::vector<std::uint8_t> blocked = computeBlockedStockNodes(eval, vacateKills, deterredShooters);
        SeqStockCacheKey fastKey{key, blocked};
        if (m_fast)
        {
            if (const auto it = m_fast->seqStockCache.find(fastKey); it != m_fast->seqStockCache.end())
                return it->second;
        }
        else if (const auto it = m_seqStockCache.find(std::pair{key, blocked}); it != m_seqStockCache.end())
        {
            return it->second;
        }
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
                const std::int32_t h = m_stockField.horizonTurns() > 0
                    ? m_stockField.horizonTurns() : DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
                const MilliFunds spawnGain = scalePaybackToRelevanceHorizon(
                    m_stockContext.futureFactorySpawnGain(key, floor, m_properties, m_knowledge.observerId(), blocked),
                    h);
                const MilliFunds strategicBonus =
                    computeStrategicCaptureBonus(eval, blocked, &floor, vacateKills, deterredShooters);
                const MilliFunds expansionGain = spawnGain > 0 ? std::max(spawnGain, strategicBonus) : strategicBonus;
                MilliFunds stockNet = owned.owned;
                if (RelativeStockDetail::checkedAdd(stockNet, floor.floor) &&
                    RelativeStockDetail::checkedSubtract(stockNet, m_stockContext.enemyOptimum()))
                {
                    MilliFunds value = scalePaybackToRelevanceHorizon(stockNet, h);
                    if (RelativeStockDetail::checkedAdd(value, expansionGain))
                    {
                        if (m_fast)
                            m_fast->seqStockCache.emplace(std::move(fastKey), value);
                        else
                            m_seqStockCache.emplace(std::pair{key, blocked}, value);
                        return value;
                    }
                }
            }
        }
        const std::int32_t h = m_stockField.horizonTurns() > 0
            ? m_stockField.horizonTurns() : DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
        const auto actions = buildPostReplyStockActions(m_problem, m_knowledge, eval);
        MilliFunds fallback = m_stockField.jointStock(actions);
        if (!m_stockContext.valid())
        {
            for (const auto & actor : eval.actors)
            {
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                const int ki = member.knowledgeUnitIndex;
                double deterredHp = 0.0;
                const int effSteps = isOnlyHitByDeterred(eval, ki, deterredShooters, deterredHp)
                    ? actor.stepsAfterOwnActions : actor.stepsAfterReply;
                if (effSteps <= 0 || std::ranges::find(vacateKills, ki) != vacateKills.end())
                    continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                if (cand.capture.has_value())
                    RelativeStockDetail::checkedAdd(fallback, openRefinementCaptureContinuation(*cand.capture, h));
            }
        }
        fallback = scalePaybackToRelevanceHorizon(fallback, h);
        RelativeStockDetail::checkedAdd(
            fallback, computeStrategicCaptureBonus(eval, blocked, nullptr, vacateKills, deterredShooters));
        if (m_fast)
            m_fast->seqStockCache.emplace(std::move(fastKey), fallback);
        else
            m_seqStockCache.emplace(std::pair{key, blocked}, fallback);
        return fallback;
    }

    MilliFunds UnifiedTeamEvaluator::computeStrategicCaptureBonus(const ReplyPassEvaluation & eval,
                                            std::span<const std::uint8_t> blocked,
                                            const ReplyStockFloor * floor,
                                            std::span<const int> vacateKills,
                                            std::span<const int> deterredShooters) const
    {
        if (m_productionModel == nullptr || !m_productionModel->valid() || m_strategicBonusTiles.empty())
            return 0;
        const std::int32_t stockHorizon = m_stockField.horizonTurns() > 0
            ? m_stockField.horizonTurns() : UNIFIED_PROD_DEFAULT_HORIZON;
        const std::int32_t horizon = std::max(stockHorizon, m_productionModel->input().horizonTurns);
        const std::int32_t capPts = m_stockField.capturePointsToCapture() > 0
            ? m_stockField.capturePointsToCapture() : UNIFIED_PROD_DEFAULT_CAP_POINTS;
        std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> witnessTurnByTile;
        if (floor != nullptr && floor->status == ReplyStockStatus::Valid && m_stockContext.valid())
        {
            for (const SequentialRowWitness & rw : floor->witness)
            {
                for (const SequentialWitnessStep & st : rw.steps)
                {
                    if (st.node >= 0 && static_cast<std::size_t>(st.node) < m_stockContext.nodeCount())
                    {
                        const TilePoint nt = m_stockContext.nodeTile(static_cast<std::size_t>(st.node));
                        auto [it, ins] = witnessTurnByTile.emplace(std::pair{nt.x, nt.y}, st.turn);
                        if (!ins && st.turn < it->second)
                            it->second = st.turn;
                    }
                }
            }
        }
        const auto isTileBlocked = [&](TilePoint tile) {
            if (!m_stockContext.valid())
                return false;
            for (std::size_t n = 0; n < m_stockContext.nodeCount(); ++n)
            {
                if (m_stockContext.nodeTile(n) == tile)
                    return n < blocked.size() && blocked[n] != 0;
            }
            return false;
        };

        const auto isTileOccupiedByEnemy = [&](TilePoint tile) {
            for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
            {
                const auto & pawn = m_local.units[static_cast<std::size_t>(ki)];
                if (!pawn.present || !pawn.enemy || pawn.tile == INVALID_TILE)
                    continue;
                if (const auto itOwn = eval.enemyAfterOwn.find(ki);
                    itOwn != eval.enemyAfterOwn.end() && itOwn->second <= 0.0)
                    continue;
                if (const auto itRep = eval.enemyReplyHp.find(ki);
                    itRep != eval.enemyReplyHp.end() && itRep->second <= 0)
                    continue;
                const TilePoint pos = (ki < static_cast<int>(eval.combatLocations.size()) &&
                                       eval.combatLocations[static_cast<std::size_t>(ki)] != INVALID_TILE)
                    ? eval.combatLocations[static_cast<std::size_t>(ki)]
                    : pawn.tile;
                if (pos == tile)
                    return true;
            }
            return false;
        };
        const bool hasValidFloor = floor != nullptr && floor->status == ReplyStockStatus::Valid && m_stockContext.valid();

        MilliFunds total = 0;
        std::vector<int> usedFallbackCappers;
        for (const auto & [tile, owner] : m_strategicBonusTiles)
        {
            (void)owner;
            if (isTileBlocked(tile) || isTileOccupiedByEnemy(tile))
                continue;
            std::int32_t bestOwnTurn = horizon;
            if (const auto wIt = witnessTurnByTile.find(std::pair{tile.x, tile.y});
                wIt != witnessTurnByTile.end())
            {
                bestOwnTurn = wIt->second;
            }
            else
            {
                int bestCapperKi = NO_UNIT;
                for (const auto & actor : eval.actors)
                {
                    const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                    const int ki = member.knowledgeUnitIndex;
                    double deterredHp = 0.0;
                    const int effSteps = isOnlyHitByDeterred(eval, ki, deterredShooters, deterredHp)
                        ? actor.stepsAfterOwnActions : actor.stepsAfterReply;
                    if (effSteps <= 0 || std::ranges::find(vacateKills, ki) != vacateKills.end())
                        continue;
                    if (member.stockRow == NO_STOCK_ROW)
                        continue;
                    if (std::ranges::find(usedFallbackCappers, ki) != usedFallbackCappers.end())
                        continue;
                    if (ki < 0 || ki >= static_cast<int>(m_knowledge.units().size()) ||
                        ki >= static_cast<int>(m_local.units.size()))
                        continue;
                    const auto & known = m_knowledge.units()[static_cast<std::size_t>(ki)];
                    const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                    const int nextRate = replyCaptureRate(
                        known.captureRate, std::max(0, known.hpRounded - effSteps), false);
                    if (cand.capture.has_value() && cand.destination == tile)
                    {
                        const int carried = (cand.destination == cand.origin ? known.capturePoints : 0) + known.captureRate;
                        if (carried >= capPts)
                        {
                            bestOwnTurn = 0;
                            bestCapperKi = ki;
                            break;
                        }
                        if (nextRate > 0)
                        {
                            const int remTurns = (capPts - carried + nextRate - 1) / nextRate;
                            if (remTurns < bestOwnTurn)
                            {
                                bestOwnTurn = remTurns;
                                bestCapperKi = ki;
                            }
                        }
                        continue;
                    }
                    // When a valid sequential floor omitted this tile, never hallucinate a speculative multi-turn HQ/site route.
                    if (hasValidFloor)
                        continue;
                    if (nextRate <= 0)
                        continue;
                    const int move = std::max(1, m_local.units[static_cast<std::size_t>(ki)].movement);
                    const int pathCost = tilePathDistance(ki, cand.destination, tile, false);
                    if (pathCost >= UNIFIED_UNREACHABLE_COST)
                        continue;
                    const int moveTurns = (pathCost + move - 1) / move;
                    const int capTurns = (capPts + nextRate - 1) / nextRate;
                    const int ownTurn = std::max(1, moveTurns) + capTurns - 1;
                    if (ownTurn < bestOwnTurn)
                    {
                        bestOwnTurn = ownTurn;
                        bestCapperKi = ki;
                    }
                }
                if (bestCapperKi != NO_UNIT)
                    usedFallbackCappers.push_back(bestCapperKi);
            }
            if (bestOwnTurn < horizon)
            {
                RelativeStockDetail::checkedAdd(
                    total, m_productionModel->combatBonusCaptureValue(tile, bestOwnTurn, horizon));
                RelativeStockDetail::checkedAdd(
                    total, m_productionModel->siteCaptureProductionSurplus(tile, bestOwnTurn, horizon));
            }
        }
        return total;
    }

    MilliFunds UnifiedTeamEvaluator::computeProductionOpenSiteValue(const ReplyPassEvaluation & eval) const
    {
        if (m_productionModel == nullptr || !m_productionModel->valid() ||
            (m_friendlyFactories.empty() && m_enemyFactories.empty()))
            return 0;

        std::vector<TilePoint> occupied;
        std::vector<TilePoint> friendlySurviving;
        occupied.reserve(m_local.units.size());
        friendlySurviving.reserve(m_local.units.size());
        std::vector<std::uint8_t> actorKiSeen(m_local.units.size(), 0);
        for (const auto & actor : eval.actors)
        {
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const int ki = member.knowledgeUnitIndex;
            if (ki >= 0 && ki < static_cast<int>(actorKiSeen.size()))
                actorKiSeen[static_cast<std::size_t>(ki)] = 1;
            if (actor.stepsAfterReply <= 0)
                continue;
            const TilePoint dest = member.candidates[static_cast<std::size_t>(actor.candidateIndex)].destination;
            occupied.push_back(dest);
            friendlySurviving.push_back(dest);
        }
        for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
        {
            if (actorKiSeen[static_cast<std::size_t>(ki)] != 0)
                continue;
            const auto & pawn = m_local.units[static_cast<std::size_t>(ki)];
            if (!pawn.present || pawn.tile == INVALID_TILE)
                continue;
            if (pawn.enemy)
            {
                if (const auto it = eval.enemyReplyHp.find(ki); it != eval.enemyReplyHp.end() && it->second <= 0)
                    continue;
                if (const auto itOwn = eval.enemyAfterOwn.find(ki);
                    itOwn != eval.enemyAfterOwn.end() && itOwn->second <= 0.0)
                    continue;
            }
            else
            {
                double hpF = pawn.realHp >= 0.0 ? pawn.realHp : static_cast<double>(UNIT_HP_STEPS);
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    hpF = it->second;
                if (hpF <= 0.0)
                    continue;
            }
            const TilePoint pos = (ki < static_cast<int>(eval.combatLocations.size()) &&
                                   eval.combatLocations[static_cast<std::size_t>(ki)] != INVALID_TILE)
                ? eval.combatLocations[static_cast<std::size_t>(ki)]
                : pawn.tile;
            occupied.push_back(pos);
            if (!pawn.enemy)
                friendlySurviving.push_back(pos);
        }

        MilliFunds totalVal = 0;
        const MilliFunds spendable = m_productionModel->currentFunds();
        if (spendable > 0 && !m_friendlyFactories.empty())
        {
            std::vector<TilePoint> openSites;
            std::uint64_t mask = 0;
            for (std::size_t i = 0; i < m_friendlyFactories.size(); ++i)
            {
                const TilePoint site = m_friendlyFactories[i];
                if (std::ranges::find(occupied, site) == occupied.end())
                {
                    openSites.push_back(site);
                    if (i < UNIFIED_OPEN_SITE_MASK_BITS)
                        mask |= (std::uint64_t{1} << i);
                }
            }
            if (!openSites.empty())
            {
                MilliFunds ownVal = 0;
                if (m_friendlyFactories.size() <= UNIFIED_OPEN_SITE_MASK_BITS)
                {
                    if (m_fast)
                    {
                        if (const auto it = m_fast->openSiteValueCache.find(mask); it != m_fast->openSiteValueCache.end())
                            ownVal = it->second;
                        else
                        {
                            ownVal = m_productionModel->marginalOpenSitesValue(openSites, spendable);
                            m_fast->openSiteValueCache.emplace(mask, ownVal);
                        }
                    }
                    else if (const auto it = m_openSiteValueCache.find(mask); it != m_openSiteValueCache.end())
                    {
                        ownVal = it->second;
                    }
                    else
                    {
                        ownVal = m_productionModel->marginalOpenSitesValue(openSites, spendable);
                        m_openSiteValueCache.emplace(mask, ownVal);
                    }
                }
                else
                {
                    ownVal = m_productionModel->marginalOpenSitesValue(openSites, spendable);
                }
                RelativeStockDetail::checkedAdd(totalVal, ownVal);
            }
        }
        for (const TilePoint enemySite : m_enemyFactories)
        {
            if (std::ranges::find(friendlySurviving, enemySite) != friendlySurviving.end())
                RelativeStockDetail::checkedAdd(totalVal, m_productionModel->enemySiteBlockDenial(enemySite));
        }
        return totalVal;
    }

    double UnifiedTeamEvaluator::maxWeaponBaseDamageUncached(int attackerKi, int defenderKi, int range) const
    {
        if (attackerKi < 0 || attackerKi >= static_cast<int>(m_local.units.size()) ||
            defenderKi < 0 || defenderKi >= static_cast<int>(m_local.units.size()))
            return 0.0;
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

    double UnifiedTeamEvaluator::maxWeaponBaseDamage(int attackerKi, int defenderKi, int range) const
    {
        if (attackerKi >= 0 && attackerKi < static_cast<int>(m_unitCount) &&
            defenderKi >= 0 && defenderKi < static_cast<int>(m_unitCount))
        {
            const std::size_t flat = static_cast<std::size_t>(attackerKi) * m_unitCount +
                static_cast<std::size_t>(defenderKi);
            if (range == 1 && flat < m_baseDmgRange1.size() && m_baseDmgRange1[flat] >= 0.0)
                return m_baseDmgRange1[flat];
            if (flat < m_baseDmgMinRange.size() &&
                range == m_unitRanges[static_cast<std::size_t>(attackerKi)].first &&
                m_baseDmgMinRange[flat] >= 0.0)
                return m_baseDmgMinRange[flat];
        }
        return maxWeaponBaseDamageUncached(attackerKi, defenderKi, range);
    }

    void UnifiedTeamEvaluator::precomputeWeaponTables()
    {
        if ((!m_local.weapon && !m_local.weaponSlots) || m_unitCount == 0)
            return;
        m_baseDmgRange1.assign(m_unitCount * m_unitCount, -1.0);
        m_baseDmgMinRange.assign(m_unitCount * m_unitCount, -1.0);
        for (std::size_t a = 0; a < m_unitCount; ++a)
        {
            if (!m_local.units[a].present)
                continue;
            const int minR = m_unitRanges[a].first;
            for (std::size_t b = 0; b < m_unitCount; ++b)
            {
                if (!m_local.units[b].present || m_local.units[a].enemy == m_local.units[b].enemy)
                    continue;
                const std::size_t flat = a * m_unitCount + b;
                const double d1 = maxWeaponBaseDamageUncached(static_cast<int>(a), static_cast<int>(b), 1);
                m_baseDmgRange1[flat] = d1;
                m_baseDmgMinRange[flat] = minR == 1
                    ? d1
                    : maxWeaponBaseDamageUncached(static_cast<int>(a), static_cast<int>(b), minR);
            }
        }
    }

    std::pair<MilliFunds, std::vector<int>> UnifiedTeamEvaluator::computeVacateRetaliationLoss(const ReplyPassEvaluation & eval) const
    {
        std::pair<MilliFunds, std::vector<int>> out{0, {}};
        if (m_local.width <= 0 || m_local.height <= 0 || (!m_local.weapon && !m_local.weaponSlots))
            return out;

        struct FriendlyVictim
        {
            int ki{NO_UNIT};
            TilePoint pos{INVALID_TILE};
            double hp{0.0};
            MilliFunds cost{0};
            bool directCounter{false};
        };
        struct EnemyUnitState
        {
            int ki{NO_UNIT};
            TilePoint pos{INVALID_TILE};
            double hp{0.0};
            int move{0};
            int grid{-1};
            int minRange{1};
            MilliFunds cost{0};
            bool firedInE0{false};
            bool canVacate{false};
            MilliFunds e0ShotNet{0};
        };

        std::vector<FriendlyVictim> friendlies;
        std::vector<TilePoint> friendlyTiles;
        std::vector<TilePoint> deadFriendlyTiles;
        std::vector<std::uint8_t> actorKiSeen(m_local.units.size(), 0);
        const auto addFriendly = [&](int ki, TilePoint pos, double hpF) {
            if (pos == INVALID_TILE)
                return;
            if (hpF <= 0.0)
            {
                deadFriendlyTiles.push_back(pos);
                return;
            }
            friendlyTiles.push_back(pos);
            if (ki < 0 || ki >= static_cast<int>(m_local.units.size()))
                return;
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            bool directCounter = fp.movement > 0;
            if (ki < static_cast<int>(m_knowledge.units().size()))
            {
                const auto & knownF = m_knowledge.units()[static_cast<std::size_t>(ki)];
                directCounter = knownF.hasWeapons && knownF.canMoveAndFire && knownF.minRange <= 1;
            }
            friendlies.push_back({ki, pos, hpF, fp.replacementCost, directCounter});
        };
        for (const auto & actor : eval.actors)
        {
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const int ki = member.knowledgeUnitIndex;
            if (ki >= 0 && ki < static_cast<int>(actorKiSeen.size()))
                actorKiSeen[static_cast<std::size_t>(ki)] = 1;
            const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
            if (actor.stepsAfterReply <= 0)
            {
                deadFriendlyTiles.push_back(cand.destination);
                continue;
            }
            double hpF = static_cast<double>(actor.stepsAfterReply);
            if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                hpF = it->second;
            addFriendly(ki, cand.destination, hpF);
        }
        for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
        {
            if (actorKiSeen[static_cast<std::size_t>(ki)] != 0)
                continue;
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            if (!fp.present || fp.enemy || fp.tile == INVALID_TILE)
                continue;
            double hpF = fp.realHp >= 0.0
                ? fp.realHp
                : static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded);
            if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                hpF = it->second;
            addFriendly(ki, fp.tile, hpF);
        }
        if (friendlies.empty())
            return out;

        std::vector<EnemyUnitState> enemies;
        std::vector<TilePoint> enemyTiles;
        for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
        {
            const auto & ep = m_local.units[static_cast<std::size_t>(e)];
            if (!ep.present || !ep.enemy)
                continue;
            double hpE = ep.realHp >= 0.0
                ? ep.realHp
                : static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(e)].hpRounded);
            if (const auto it = eval.enemyReplyHp.find(e); it != eval.enemyReplyHp.end())
                hpE = static_cast<double>(it->second);
            else if (const auto itOwn = eval.enemyAfterOwn.find(e); itOwn != eval.enemyAfterOwn.end())
                hpE = static_cast<double>(itOwn->second);
            if (hpE <= 0.0)
                continue;
            const TilePoint pos = (e < static_cast<int>(eval.combatLocations.size()) &&
                                   eval.combatLocations[static_cast<std::size_t>(e)] != INVALID_TILE)
                ? eval.combatLocations[static_cast<std::size_t>(e)]
                : ep.tile;
            if (pos == INVALID_TILE)
                continue;
            const bool fired = std::ranges::any_of(eval.shots, [&](const ReplyPassShot & s) {
                return s.shooterIndex == e;
            });
            MilliFunds shotNet = 0;
            for (const ReplyPassShot & s : eval.shots)
            {
                if (s.shooterIndex == e)
                    shotNet += std::max<MilliFunds>(0, s.targetCapital - s.counterCapital);
            }
            const int minR = (e < static_cast<int>(m_knowledge.units().size()))
                ? std::max(1, m_knowledge.units()[static_cast<std::size_t>(e)].minRange)
                : 1;
            enemyTiles.push_back(pos);
            enemies.push_back({e, pos, hpE, std::max(0, ep.movement), pawnTravelGrid(ep), minR,
                               ep.replacementCost, fired, false, shotNet});
        }
        if (enemies.empty())
            return out;

        const std::uint32_t occFriendlyGen = nextOccFriendlyGen();
        const std::uint32_t occEnemyGen = nextOccEnemyGen();
        for (const auto & f : friendlies)
        {
            if (f.pos.x >= 0 && f.pos.x < m_local.width && f.pos.y >= 0 && f.pos.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(f.pos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(f.pos.x);
                m_occFriendlyGen[cIdx] = occFriendlyGen;
                m_occFriendlyKi[cIdx] = f.ki;
            }
        }
        for (const TilePoint t : friendlyTiles)
        {
            if (t.x >= 0 && t.x < m_local.width && t.y >= 0 && t.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(t.x);
                m_occFriendlyGen[cIdx] = occFriendlyGen;
            }
        }
        for (const auto & en : enemies)
        {
            if (en.pos.x >= 0 && en.pos.x < m_local.width && en.pos.y >= 0 && en.pos.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(en.pos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(en.pos.x);
                m_occEnemyGen[cIdx] = occEnemyGen;
                m_occEnemyKi[cIdx] = en.ki;
            }
        }

        const auto isOccupiedByFriendly = [&](TilePoint t, TilePoint ignoredFriendly) {
            if (t.x < 0 || t.x >= m_local.width || t.y < 0 || t.y >= m_local.height)
                return false;
            const std::size_t cIdx = static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(t.x);
            return m_occFriendlyGen[cIdx] == occFriendlyGen && t != ignoredFriendly;
        };
        const auto isOccupiedByAny = [&](TilePoint t) {
            if (t.x < 0 || t.x >= m_local.width || t.y < 0 || t.y >= m_local.height)
                return false;
            const std::size_t cIdx = static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(t.x);
            return m_occFriendlyGen[cIdx] == occFriendlyGen || m_occEnemyGen[cIdx] == occEnemyGen;
        };
        const auto enemyCanStepAside = [&](const EnemyUnitState & occ) {
            if (occ.move <= 0)
                return false;
            const MobilityCostGrid * gridPtr = (occ.grid >= 0 && occ.grid < static_cast<int>(m_local.grids.size()))
                ? &m_local.grids[static_cast<std::size_t>(occ.grid)]
                : nullptr;
            for (const EntryDirection dir : ENTRY_DIRECTIONS)
            {
                const StepVector step = entryStep(dir);
                const TilePoint stepTile{occ.pos.x + step.dx, occ.pos.y + step.dy};
                if (stepTile.x < 0 || stepTile.x >= m_local.width ||
                    stepTile.y < 0 || stepTile.y >= m_local.height)
                    continue;
                if (isOccupiedByAny(stepTile))
                    continue;
                if (gridPtr != nullptr)
                {
                    const std::int32_t c = gridPtr->cost(stepTile.x, stepTile.y, dir);
                    if (c == IMPASSABLE_COST || c > occ.move)
                        continue;
                }
                return true;
            }
            return false;
        };

        // Any mobile enemy adjacent to a friendly can move aside rather than shield behind its greedy E0 shot.
        for (auto & blocker : enemies)
        {
            if (blocker.move <= 0)
                continue;
            const bool adjacentToFriendly = std::ranges::any_of(friendlies, [&](const FriendlyVictim & f) {
                return manhattanDistance(blocker.pos, f.pos) == 1;
            });
            if (!adjacentToFriendly)
                continue;
            blocker.canVacate = enemyCanStepAside(blocker);
        }

        const auto attackerCanReachTile = [&](const EnemyUnitState & att, TilePoint goal,
                                              TilePoint ignoredFriendly) {
            return boundedCanReachGoal(att.ki, att.grid, att.pos, goal, att.move,
                                       occFriendlyGen, ignoredFriendly, true);
        };
        const auto directTradeFrom = [&](const EnemyUnitState & att, TilePoint fromTile,
                                         const FriendlyVictim & victim, double victimHp) -> std::pair<MilliFunds, double> {
            const double baseDmg = maxWeaponBaseDamage(att.ki, victim.ki, 1);
            if (baseDmg <= 0.0)
                return {0, victimHp};
            const TerrainDefenseRule vDef = m_local.defense
                ? m_local.defense(victim.ki, victim.pos)
                : TerrainDefenseRule{};
            const double rawHit = terrainReducedDamage(vDef, baseDmg, att.hp, ceilHpSteps(victimHp));
            const double hit = engineShotDamage(rawHit);
            if (hit <= 0.0)
                return {0, victimHp};
            const double afterV = engineHpAfterReply(victimHp, hit);
            const MilliFunds vLoss = combatCapitalLoss(victim.cost, victimHp, afterV);
            MilliFunds eCounterLoss = 0;
            if (victim.directCounter && afterV > 0.0)
            {
                const double cBase = maxWeaponBaseDamage(victim.ki, att.ki, 1);
                if (cBase > 0.0)
                {
                    const TerrainDefenseRule aDef = m_local.defense
                        ? m_local.defense(att.ki, fromTile)
                        : TerrainDefenseRule{};
                    const double rawC = sameActionCounterDamage(
                        aDef, cBase, victimHp, rawHit, ceilHpSteps(att.hp));
                    const double afterA = engineHpAfterReply(att.hp, engineShotDamage(rawC));
                    eCounterLoss = combatCapitalLoss(att.cost, att.hp, afterA);
                }
            }
            return {vLoss - eCounterLoss, afterV};
        };

        static constexpr std::size_t MAX_WALLBREAK_ENEMY_KILLERS = 3;
        struct VacateStrike
        {
            std::size_t attackerIdx{0};
            std::array<std::size_t, MAX_WALLBREAK_ENEMY_KILLERS> killerEnemyIndices{
                std::numeric_limits<std::size_t>::max(),
                std::numeric_limits<std::size_t>::max(),
                std::numeric_limits<std::size_t>::max()};
            std::size_t killerCount{0};
            std::size_t occEnemyIdx{std::numeric_limits<std::size_t>::max()};
            TilePoint openedTile{INVALID_TILE};
            std::size_t screenFriendlyIdx{std::numeric_limits<std::size_t>::max()};
            std::size_t victimIdx{0};
            MilliFunds netGain{0};
            MilliFunds killerCostOffset{0};
            double victimAfterHp{0.0};
        };
        std::vector<VacateStrike> strikes;

        if (enemies.size() >= 2)
        {
            for (std::size_t bi = 0; bi < enemies.size(); ++bi)
            {
                const auto & blocker = enemies[bi];
                if (!blocker.canVacate)
                    continue;
                for (std::size_t fi = 0; fi < friendlies.size(); ++fi)
                {
                    const auto & victim = friendlies[fi];
                    if (manhattanDistance(blocker.pos, victim.pos) != 1)
                        continue;
                    for (std::size_t ei = 0; ei < enemies.size(); ++ei)
                    {
                        if (ei == bi)
                            continue;
                        const auto & att = enemies[ei];
                        if (att.firedInE0 || att.minRange > 1 || att.move <= 0 || att.pos == blocker.pos)
                            continue;
                        if (!attackerCanReachTile(att, blocker.pos, INVALID_TILE))
                            continue;
                        const auto [net, afterV] = directTradeFrom(att, blocker.pos, victim, victim.hp);
                        const MilliFunds netGain = net - blocker.e0ShotNet;
                        if (netGain > 0)
                        {
                            VacateStrike st;
                            st.attackerIdx = ei;
                            st.killerEnemyIndices[0] = bi;
                            st.killerCount = 1;
                            st.openedTile = blocker.pos;
                            st.victimIdx = fi;
                            st.netGain = netGain;
                            st.killerCostOffset = blocker.e0ShotNet;
                            st.victimAfterHp = afterV;
                            strikes.push_back(st);
                        }
                    }
                }
            }
        }

        // Case 2a: Friendly unit already died in E0, opening its tile for an unspent direct enemy attacker.
        for (const TilePoint deadTile : deadFriendlyTiles)
        {
            for (std::size_t fi = 0; fi < friendlies.size(); ++fi)
            {
                const auto & victim = friendlies[fi];
                if (manhattanDistance(deadTile, victim.pos) != 1)
                    continue;
                for (std::size_t ei = 0; ei < enemies.size(); ++ei)
                {
                    const auto & att = enemies[ei];
                    if (att.firedInE0 || att.minRange > 1 || att.move <= 0)
                        continue;
                    if (!attackerCanReachTile(att, deadTile, INVALID_TILE))
                        continue;
                    const auto [net, afterV] = directTradeFrom(att, deadTile, victim, victim.hp);
                    if (net > 0)
                    {
                        VacateStrike st;
                        st.attackerIdx = ei;
                        st.openedTile = deadTile;
                        st.victimIdx = fi;
                        st.netGain = net;
                        st.victimAfterHp = afterV;
                        strikes.push_back(st);
                    }
                }
            }
        }

        // Case 2b: Mirrored 1-to-3 unit enemy wallbreak across up to 3 clear adjacent spaces, opening screen.pos.
        if (enemies.size() >= 2)
        {
            for (std::size_t si = 0; si < friendlies.size(); ++si)
            {
                const auto & screen = friendlies[si];
                const bool shieldsAnother = std::ranges::any_of(friendlies, [&](const FriendlyVictim & v) {
                    return v.pos != screen.pos && manhattanDistance(screen.pos, v.pos) == 1;
                });
                if (!shieldsAnother)
                    continue;
                const TerrainDefenseRule sDef = m_local.defense
                    ? m_local.defense(screen.ki, screen.pos)
                    : TerrainDefenseRule{};

                struct ScreenKillerOpt
                {
                    std::size_t enemyIdx{0};
                    TilePoint strikeTile{INVALID_TILE};
                    bool consumesAdjTile{false};
                    std::size_t occIdx{std::numeric_limits<std::size_t>::max()};
                    MilliFunds forgoCost{0};
                    double baseDmg{0.0};
                    double maxHitHp{0.0};
                };
                std::vector<ScreenKillerOpt> killerOpts;
                for (std::size_t ki = 0; ki < enemies.size(); ++ki)
                {
                    const auto & killer = enemies[ki];
                    const int maxR = (killer.ki < static_cast<int>(m_knowledge.units().size()))
                        ? std::max(1, m_knowledge.units()[static_cast<std::size_t>(killer.ki)].maxRange)
                        : 1;
                    const double kBase = maxWeaponBaseDamage(killer.ki, screen.ki, killer.minRange);
                    if (kBase <= 0.0)
                        continue;
                    const double rawK = terrainReducedDamage(sDef, kBase, killer.hp, ceilHpSteps(screen.hp));
                    const double hitHp = engineShotDamage(rawK);
                    if (hitHp <= 0.0)
                        continue;
                    if (killer.minRange > 1)
                    {
                        const int dist = manhattanDistance(killer.pos, screen.pos);
                        if (dist >= killer.minRange && dist <= maxR)
                            killerOpts.push_back({ki, killer.pos, false,
                                                  std::numeric_limits<std::size_t>::max(),
                                                  killer.e0ShotNet, kBase, hitHp});
                    }
                    else
                    {
                        for (const EntryDirection dir : ENTRY_DIRECTIONS)
                        {
                            const StepVector step = entryStep(dir);
                            const TilePoint adj{screen.pos.x + step.dx, screen.pos.y + step.dy};
                            if (adj.x < 0 || adj.x >= m_local.width || adj.y < 0 || adj.y >= m_local.height)
                                continue;
                            if (isOccupiedByFriendly(adj, INVALID_TILE))
                                continue;
                            std::size_t occIdx = std::numeric_limits<std::size_t>::max();
                            for (std::size_t oi = 0; oi < enemies.size(); ++oi)
                            {
                                if (oi != ki && enemies[oi].pos == adj)
                                {
                                    occIdx = oi;
                                    break;
                                }
                            }
                            if (occIdx != std::numeric_limits<std::size_t>::max() &&
                                !enemyCanStepAside(enemies[occIdx]))
                                continue;
                            if (attackerCanReachTile(killer, adj, INVALID_TILE))
                            {
                                const MilliFunds occForgo = occIdx != std::numeric_limits<std::size_t>::max()
                                    ? enemies[occIdx].e0ShotNet : 0;
                                killerOpts.push_back({ki, adj, true, occIdx,
                                                      killer.e0ShotNet + occForgo, kBase, hitHp});
                            }
                        }
                    }
                }
                if (killerOpts.empty())
                    continue;

                // Group options per enemy killer and keep top candidates by screen hit damage.
                std::vector<std::size_t> uniqueKillers;
                for (const auto & ko : killerOpts)
                {
                    if (std::ranges::find(uniqueKillers, ko.enemyIdx) == uniqueKillers.end())
                        uniqueKillers.push_back(ko.enemyIdx);
                }
                std::stable_sort(uniqueKillers.begin(), uniqueKillers.end(), [&](std::size_t a, std::size_t b) {
                    double da = 0.0, db = 0.0;
                    for (const auto & ko : killerOpts)
                    {
                        if (ko.enemyIdx == a) da = std::max(da, ko.maxHitHp);
                        if (ko.enemyIdx == b) db = std::max(db, ko.maxHitHp);
                    }
                    return da > db;
                });
                constexpr std::size_t MAX_MIRROR_KILLER_POOL = 6;
                if (uniqueKillers.size() > MAX_MIRROR_KILLER_POOL)
                    uniqueKillers.resize(MAX_MIRROR_KILLER_POOL);

                // Find cheapest 1..3 enemy wallbreak combo (distinct clear adjacent tiles) excluding breakthrough attacker ei.
                const auto findBestEnemyWallbreak = [&](std::size_t excludeAttIdx,
                                                        std::array<std::size_t, MAX_WALLBREAK_ENEMY_KILLERS> & outKillers,
                                                        std::size_t & outCount,
                                                        std::size_t & outOccIdx,
                                                        MilliFunds & outCostOffset) -> bool {
                    bool found = false;
                    MilliFunds bestCost = std::numeric_limits<MilliFunds>::max();
                    std::array<std::size_t, MAX_WALLBREAK_ENEMY_KILLERS> curKillers{};
                    std::array<TilePoint, MAX_WALLBREAK_ENEMY_KILLERS> curAdj{};
                    std::size_t curAdjCount = 0;

                    const auto dfs = [&](auto & self, std::size_t startU, std::size_t depth,
                                         double remScreenHp, MilliFunds curCost, std::size_t curOcc) -> void {
                        if (found && curCost >= bestCost)
                            return;
                        if (remScreenHp <= 0.0)
                        {
                            found = true;
                            bestCost = curCost;
                            outKillers = curKillers;
                            outCount = depth;
                            outOccIdx = curOcc;
                            outCostOffset = curCost;
                            return;
                        }
                        if (depth >= MAX_WALLBREAK_ENEMY_KILLERS || startU >= uniqueKillers.size())
                            return;
                        for (std::size_t ui = startU; ui < uniqueKillers.size(); ++ui)
                        {
                            const std::size_t eIdx = uniqueKillers[ui];
                            if (eIdx == excludeAttIdx || eIdx == curOcc)
                                continue;
                            const auto & kEnemy = enemies[eIdx];
                            for (const auto & ko : killerOpts)
                            {
                                if (ko.enemyIdx != eIdx)
                                    continue;
                                if (ko.occIdx == excludeAttIdx)
                                    continue;
                                if (ko.occIdx != std::numeric_limits<std::size_t>::max())
                                {
                                    bool occUsed = false;
                                    for (std::size_t j = 0; j < depth; ++j)
                                        if (curKillers[j] == ko.occIdx) occUsed = true;
                                    if (occUsed)
                                        continue;
                                }
                                if (ko.consumesAdjTile)
                                {
                                    bool tileUsed = false;
                                    for (std::size_t j = 0; j < curAdjCount; ++j)
                                        if (curAdj[j] == ko.strikeTile) tileUsed = true;
                                    if (tileUsed)
                                        continue;
                                }
                                const double rawHit = terrainReducedDamage(
                                    sDef, ko.baseDmg, kEnemy.hp, ceilHpSteps(remScreenHp));
                                const double hit = engineShotDamage(rawHit);
                                if (hit <= 0.0)
                                    continue;
                                const double afterS = engineHpAfterReply(remScreenHp, hit);
                                MilliFunds counterLoss = 0;
                                if (screen.directCounter && kEnemy.minRange <= 1 && afterS > 0.0)
                                {
                                    const double cBase = maxWeaponBaseDamage(screen.ki, kEnemy.ki, 1);
                                    if (cBase > 0.0)
                                    {
                                        const TerrainDefenseRule aDef = m_local.defense
                                            ? m_local.defense(kEnemy.ki, ko.strikeTile)
                                            : TerrainDefenseRule{};
                                        const double rawC = sameActionCounterDamage(
                                            aDef, cBase, remScreenHp, rawHit, ceilHpSteps(kEnemy.hp));
                                        counterLoss = combatCapitalLoss(
                                            kEnemy.cost, kEnemy.hp,
                                            engineHpAfterReply(kEnemy.hp, engineShotDamage(rawC)));
                                    }
                                }
                                const MilliFunds nextCost = curCost + ko.forgoCost + counterLoss;
                                if (found && nextCost >= bestCost)
                                    continue;
                                curKillers[depth] = eIdx;
                                const std::size_t prevAdj = curAdjCount;
                                if (ko.consumesAdjTile)
                                    curAdj[curAdjCount++] = ko.strikeTile;
                                const std::size_t nextOcc = curOcc != std::numeric_limits<std::size_t>::max()
                                    ? curOcc : ko.occIdx;
                                self(self, ui + 1, depth + 1, afterS, nextCost, nextOcc);
                                curAdjCount = prevAdj;
                            }
                        }
                    };
                    dfs(dfs, 0, 0, screen.hp, 0, std::numeric_limits<std::size_t>::max());
                    return found;
                };

                const MilliFunds screenLoss = combatCapitalLoss(screen.cost, screen.hp, 0.0);
                for (std::size_t fi = 0; fi < friendlies.size(); ++fi)
                {
                    if (fi == si)
                        continue;
                    const auto & victim = friendlies[fi];
                    if (manhattanDistance(screen.pos, victim.pos) != 1)
                        continue;
                    for (std::size_t ei = 0; ei < enemies.size(); ++ei)
                    {
                        const auto & att = enemies[ei];
                        if (att.firedInE0 || att.minRange > 1 || att.move <= 0)
                            continue;
                        if (!attackerCanReachTile(att, screen.pos, screen.pos))
                            continue;
                        std::array<std::size_t, MAX_WALLBREAK_ENEMY_KILLERS> kIds{
                            std::numeric_limits<std::size_t>::max(),
                            std::numeric_limits<std::size_t>::max(),
                            std::numeric_limits<std::size_t>::max()};
                        std::size_t kCnt = 0;
                        std::size_t kOcc = std::numeric_limits<std::size_t>::max();
                        MilliFunds kCost = 0;
                        if (!findBestEnemyWallbreak(ei, kIds, kCnt, kOcc, kCost))
                            continue;
                        const auto [net, afterV] = directTradeFrom(att, screen.pos, victim, victim.hp);
                        const MilliFunds totalGain = screenLoss + net - kCost;
                        if (totalGain > 0)
                        {
                            VacateStrike st;
                            st.attackerIdx = ei;
                            st.killerEnemyIndices = kIds;
                            st.killerCount = kCnt;
                            st.occEnemyIdx = kOcc;
                            st.openedTile = screen.pos;
                            st.screenFriendlyIdx = si;
                            st.victimIdx = fi;
                            st.netGain = totalGain;
                            st.killerCostOffset = kCost;
                            st.victimAfterHp = afterV;
                            strikes.push_back(st);
                        }
                    }
                }
            }
        }
        if (strikes.empty())
            return out;

        std::ranges::sort(strikes, [](const VacateStrike & a, const VacateStrike & b) {
            return a.netGain > b.netGain;
        });
        std::vector<std::uint8_t> enemyUsed(enemies.size(), 0);
        std::vector<TilePoint> openedUsed;
        std::vector<double> victimRemHp(friendlies.size(), 0.0);
        for (std::size_t i = 0; i < friendlies.size(); ++i)
            victimRemHp[i] = friendlies[i].hp;

        for (const auto & st : strikes)
        {
            if (enemyUsed[st.attackerIdx] || victimRemHp[st.victimIdx] <= 0.0)
                continue;
            bool killerBusy = false;
            for (std::size_t k = 0; k < st.killerCount; ++k)
            {
                if (st.killerEnemyIndices[k] < enemies.size() && enemyUsed[st.killerEnemyIndices[k]])
                {
                    killerBusy = true;
                    break;
                }
            }
            if (killerBusy)
                continue;
            if (st.occEnemyIdx < enemies.size() && enemyUsed[st.occEnemyIdx])
                continue;
            if (st.screenFriendlyIdx < friendlies.size() && victimRemHp[st.screenFriendlyIdx] <= 0.0)
                continue;
            if (std::ranges::find(openedUsed, st.openedTile) != openedUsed.end())
                continue;
            MilliFunds gain = st.netGain;
            double nextHp = st.victimAfterHp;
            if (victimRemHp[st.victimIdx] < friendlies[st.victimIdx].hp)
            {
                const auto [reNet, reAfter] = directTradeFrom(
                    enemies[st.attackerIdx], st.openedTile, friendlies[st.victimIdx], victimRemHp[st.victimIdx]);
                const MilliFunds sLoss = st.screenFriendlyIdx < friendlies.size()
                    ? combatCapitalLoss(friendlies[st.screenFriendlyIdx].cost, victimRemHp[st.screenFriendlyIdx], 0.0)
                    : 0;
                if (sLoss + reNet - st.killerCostOffset <= 0)
                    continue;
                gain = sLoss + reNet - st.killerCostOffset;
                nextHp = reAfter;
            }
            enemyUsed[st.attackerIdx] = 1;
            for (std::size_t k = 0; k < st.killerCount; ++k)
            {
                if (st.killerEnemyIndices[k] < enemies.size())
                    enemyUsed[st.killerEnemyIndices[k]] = 1;
            }
            if (st.occEnemyIdx < enemies.size())
                enemyUsed[st.occEnemyIdx] = 1;
            if (st.screenFriendlyIdx < friendlies.size())
            {
                victimRemHp[st.screenFriendlyIdx] = 0.0;
                out.second.push_back(friendlies[st.screenFriendlyIdx].ki);
            }
            openedUsed.push_back(st.openedTile);
            victimRemHp[st.victimIdx] = nextHp;
            if (nextHp <= 0.0)
                out.second.push_back(friendlies[st.victimIdx].ki);
            RelativeStockDetail::checkedAdd(out.first, gain);
        }
        return out;
    }

    MilliFunds UnifiedTeamEvaluator::computeFollowupTrapLoss(const ReplyPassEvaluation & eval,
                                       std::span<const int> vacateKills) const
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
            MilliFunds cost{0};
        };
        std::vector<EscapeVictim> victims;
        std::vector<FriendlyCoverUnit> friendlyCover;
        std::vector<std::pair<int, TilePoint>> friendlyOccupied;
        for (const auto & actor : eval.actors)
        {
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const int ki = member.knowledgeUnitIndex;
            if (actor.stepsAfterReply <= 0 || std::ranges::find(vacateKills, ki) != vacateKills.end()) continue;
            if (ki < 0 || ki >= static_cast<int>(m_local.units.size())) continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
            friendlyOccupied.emplace_back(ki, cand.destination);
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
                friendlyCover.push_back({ki, cand.destination, hp, moveB, minR, maxR, fp.replacementCost});
            }
            const int initialSteps = knownF.hpRounded;
            if (actor.stepsAfterReply < initialSteps)
            {
                if (member.stockRow != NO_STOCK_ROW && cand.capture.has_value()) continue;
                victims.push_back({ki, cand.destination, hp, actor.stepsAfterReply, true});
            }
            else if (member.stockRow == NO_STOCK_ROW)
            {
                const bool nearEnemyFactory = m_productionModel != nullptr &&
                    std::ranges::any_of(m_enemyFactories, [&](TilePoint f) {
                        const auto counter = m_productionModel->bestEnemyCounterFromSite(f, ki);
                        if (!counter)
                            return false;
                        const int cMove = counter->canMoveAndFire ? counter->movementPoints : 0;
                        return canSpawnReachAttack(
                            counter->gridId, cMove, counter->minRange, counter->maxRange,
                            f, cand.destination);
                    });
                const bool repositionedIndirect = knownF.minRange > 1 && cand.destination != cand.origin;
                if (nearEnemyFactory || repositionedIndirect)
                    victims.push_back({ki, cand.destination, hp, actor.stepsAfterReply, false});
            }
        }
        for (const auto & obs : m_local.observedDefenders)
        {
            const int ki = obs.knowledgeIndex;
            if (std::ranges::find(vacateKills, ki) != vacateKills.end()) continue;
            const auto it = eval.realAfterReply.find(ki);
            if (it == eval.realAfterReply.end() || it->second <= 0.0)
                continue;
            if (ki < 0 || ki >= static_cast<int>(m_local.units.size()) ||
                ki >= static_cast<int>(m_knowledge.units().size()))
                continue;
            friendlyOccupied.emplace_back(ki, obs.tile);
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            const auto & knownObs = m_knowledge.units()[static_cast<std::size_t>(ki)];
            if (obs.stockRow == NO_STOCK_ROW)
            {
                const int minR = std::max(1, knownObs.minRange);
                const int maxR = std::max(1, knownObs.maxRange);
                const int moveB = minR > 1 ? 0 : std::max(0, fp.movement);
                friendlyCover.push_back({ki, obs.tile, it->second, moveB, minR, maxR, fp.replacementCost});
            }
            const int beforeSteps = obs.facts ? ceilHpSteps(obs.facts->actorRealHp) : knownObs.hpRounded;
            const int afterSteps = ceilHpSteps(it->second);
            if (afterSteps < beforeSteps)
                victims.push_back({ki, obs.tile, it->second, afterSteps, true});
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
            // Symmetric 1-turn pursuit vs 1-turn F1 escape so healthy units do not retreat from phantom double moves.
            const int oneTurnMove = minR > 1 ? 0 : moveE;
            const int damagedMove = minR > 1 ? (firedE0 ? 0 : moveE) : oneTurnMove;
            threats.push_back({e, posE, hpE, damagedMove, oneTurnMove, minR, maxR,
                               ep.capper, ep.replacementCost});
        }
        if (threats.empty()) return 0;

        std::vector<TilePoint> openEnemyFactories;
        for (const TilePoint f : m_enemyFactories)
        {
            const bool busy =
                std::ranges::any_of(enemyTiles, [&](TilePoint t) { return t == f; }) ||
                std::ranges::any_of(friendlyOccupied, [&](const auto & ft) { return ft.second == f; });
            if (!busy) openEnemyFactories.push_back(f);
        }
        const std::uint32_t occEnemyGen = nextOccEnemyGen();
        for (const auto & th : threats)
        {
            if (th.pos.x >= 0 && th.pos.x < m_local.width && th.pos.y >= 0 && th.pos.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(th.pos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(th.pos.x);
                m_occEnemyGen[cIdx] = occEnemyGen;
                m_occEnemyKi[cIdx] = th.ki;
            }
        }
        const std::uint32_t occFriendlyGen = nextOccFriendlyGen();
        for (const auto & ft : friendlyOccupied)
        {
            if (ft.second.x >= 0 && ft.second.x < m_local.width &&
                ft.second.y >= 0 && ft.second.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(ft.second.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(ft.second.x);
                m_occFriendlyGen[cIdx] = occFriendlyGen;
                m_occFriendlyKi[cIdx] = ft.first;
            }
        }
        const auto coverDetersThreat = [&](const FriendlyCoverUnit & fc, const EnemyThreat & th) {
            if (fc.ki == th.ki)
                return false;
            const double baseD = maxWeaponBaseDamage(fc.ki, th.ki, fc.strikeRange);
            if (baseD <= 0.0)
                return false;
            const TerrainDefenseRule thDef = m_local.defense ? m_local.defense(th.ki, th.pos) : TerrainDefenseRule{};
            const double rawH = terrainReducedDamage(thDef, baseD, fc.hp, ceilHpSteps(th.hp));
            const double hit = engineShotDamage(rawH);
            if (hit <= 0.0)
                return false;
            const double thAfter = engineHpAfterReply(th.hp, hit);
            const MilliFunds thLoss = combatCapitalLoss(th.cost, th.hp, thAfter);
            MilliFunds fcCounter = 0;
            if (fc.strikeRange == 1 && th.strikeRange == 1 && thAfter > 0.0)
            {
                const double cB = maxWeaponBaseDamage(th.ki, fc.ki, 1);
                if (cB > 0.0)
                {
                    const TerrainDefenseRule fcDef = m_local.defense ? m_local.defense(fc.ki, fc.pos) : TerrainDefenseRule{};
                    const double rawC = sameActionCounterDamage(fcDef, cB, th.hp, rawH, ceilHpSteps(fc.hp));
                    fcCounter = combatCapitalLoss(fc.cost, fc.hp, engineHpAfterReply(fc.hp, engineShotDamage(rawC)));
                }
            }
            return thLoss > fcCounter;
        };
        const std::size_t fcCount = friendlyCover.size();
        const std::size_t thCount = threats.size();
        std::vector<std::uint8_t> fcDetersTh(fcCount * thCount, 0);
        for (std::size_t fci = 0; fci < fcCount; ++fci)
        {
            for (std::size_t ti = 0; ti < thCount; ++ti)
            {
                if (coverDetersThreat(friendlyCover[fci], threats[ti]))
                    fcDetersTh[fci * thCount + ti] = 1;
            }
        }

        std::vector<std::uint8_t> threatSpent(threats.size(), 0);
        std::ranges::sort(victims, [&](const EscapeVictim & a, const EscapeVictim & b) {
            return m_local.units[static_cast<std::size_t>(a.ki)].replacementCost >
                m_local.units[static_cast<std::size_t>(b.ki)].replacementCost;
        });

        struct ShotPlan
        {
            std::size_t threatIdx{std::numeric_limits<std::size_t>::max()};
            double baseDmg{0.0};
            double attackerHp{0.0};
            double counterBase{0.0};
            int strikeRange{1};
            MilliFunds attackerCost{0};
            int attackerKi{NO_UNIT};
            std::uint8_t adjMask{0};
        };
        std::vector<ShotPlan> shotsAtQ;
        std::vector<std::size_t> usedHere;
        std::vector<std::size_t> bestUsedThreats;
        MilliFunds totalTrapLoss = 0;
        for (const auto & v : victims)
        {
            const auto & pawn = m_local.units[static_cast<std::size_t>(v.ki)];
            const int gridId = pawnTravelGrid(pawn);
            if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()) || pawn.movement <= 0)
                continue;
            collectReachableEscapeTiles(v.ki, gridId, v.tile, pawn.movement, pawn.ignoresCollision, occEnemyGen);
            if (m_escapeTilesScratch.empty()) continue;
            // Exact zero-loss fast probe from furthest escape tiles first (0 is global minimum with empty usedThreats).
            bool foundZeroEscape = false;
            for (auto it = m_escapeTilesScratch.rbegin(); it != m_escapeTilesScratch.rend(); ++it)
            {
                const TilePoint q = *it;
                const TilePoint ignoredFriendly = q != v.tile ? v.tile : INVALID_TILE;
                bool anyUncoveredThreat = false;
                for (std::size_t ti = 0; ti < threats.size(); ++ti)
                {
                    if (threatSpent[ti] != 0)
                        continue;
                    const auto & th = threats[ti];
                    const double baseDmg = maxWeaponBaseDamage(th.ki, v.ki, th.strikeRange);
                    if (baseDmg <= 0.0)
                        continue;
                    const int moveB = v.damaged ? th.damagedMove : th.oneTurnMove;
                    bool reaches = false;
                    if (th.strikeRange == 1)
                    {
                        for (const EntryDirection dir : ENTRY_DIRECTIONS)
                        {
                            const StepVector step = entryStep(dir);
                            const TilePoint adj{q.x + step.dx, q.y + step.dy};
                            if (canEnemyReachFiringSquare(th.ki, th.pos, adj, moveB, occFriendlyGen, ignoredFriendly))
                            {
                                reaches = true;
                                break;
                            }
                        }
                    }
                    else
                    {
                        reaches = canEnemyReachAttackOccupied(
                            th.ki, th.pos, q, moveB, th.strikeRange, th.maxRange, occFriendlyGen, ignoredFriendly);
                    }
                    if (!reaches)
                        continue;
                    if (q != v.tile || !v.damaged)
                    {
                        bool covered = false;
                        for (std::size_t fci = 0; fci < fcCount; ++fci)
                        {
                            if (fcDetersTh[fci * thCount + ti] != 0 &&
                                canFriendlyReachAttackOccupied(
                                    friendlyCover[fci].ki, friendlyCover[fci].pos, q,
                                    friendlyCover[fci].moveBudget, friendlyCover[fci].strikeRange,
                                    friendlyCover[fci].maxRange, occEnemyGen))
                            {
                                covered = true;
                                break;
                            }
                        }
                        if (covered)
                            continue;
                    }
                    anyUncoveredThreat = true;
                    break;
                }
                if (!anyUncoveredThreat)
                {
                    foundZeroEscape = true;
                    break;
                }
            }
            if (foundZeroEscape)
                continue;
            MilliFunds minEscapeLoss = std::numeric_limits<MilliFunds>::max();
            bestUsedThreats.clear();
            for (const TilePoint q : m_escapeTilesScratch)
            {
                const TilePoint ignoredFriendly = q != v.tile ? v.tile : INVALID_TILE;
                const TerrainDefenseRule defRule = m_local.defense ? m_local.defense(v.ki, q) : TerrainDefenseRule{};
                shotsAtQ.clear();
                bool hasCombatUnitThreat = false;
                for (std::size_t ti = 0; ti < threats.size(); ++ti)
                {
                    if (threatSpent[ti] != 0)
                        continue;
                    const auto & th = threats[ti];
                    const double baseDmg = maxWeaponBaseDamage(th.ki, v.ki, th.strikeRange);
                    if (baseDmg <= 0.0) continue;
                    const int moveB = v.damaged ? th.damagedMove : th.oneTurnMove;
                    std::uint8_t adjMask = 0;
                    if (th.strikeRange == 1)
                    {
                        for (std::uint8_t dirIdx = 0; dirIdx < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++dirIdx)
                        {
                            const StepVector step = entryStep(ENTRY_DIRECTIONS[dirIdx]);
                            const TilePoint adj{q.x + step.dx, q.y + step.dy};
                            if (canEnemyReachFiringSquare(th.ki, th.pos, adj, moveB, occFriendlyGen, ignoredFriendly))
                                adjMask |= static_cast<std::uint8_t>(1u << dirIdx);
                        }
                        if (adjMask == 0)
                            continue;
                    }
                    else if (!canEnemyReachAttackOccupied(th.ki, th.pos, q, moveB, th.strikeRange, th.maxRange,
                                                          occFriendlyGen, ignoredFriendly))
                    {
                        continue;
                    }
                    if (v.damaged && q != v.tile)
                    {
                        bool coveredByFriendly = false;
                        for (std::size_t fci = 0; fci < fcCount; ++fci)
                        {
                            if (fcDetersTh[fci * thCount + ti] != 0 &&
                                canFriendlyReachAttackOccupied(
                                    friendlyCover[fci].ki, friendlyCover[fci].pos, q,
                                    friendlyCover[fci].moveBudget, friendlyCover[fci].strikeRange,
                                    friendlyCover[fci].maxRange, occEnemyGen))
                            {
                                coveredByFriendly = true;
                                break;
                            }
                        }
                        if (coveredByFriendly) continue;
                    }
                    if (!th.capper) hasCombatUnitThreat = true;
                    const double cBase = th.strikeRange == 1 ? maxWeaponBaseDamage(v.ki, th.ki, 1) : 0.0;
                    shotsAtQ.push_back({ti, baseDmg, th.hp, cBase, th.strikeRange, th.cost, th.ki, adjMask});
                }
                if (!v.damaged && !shotsAtQ.empty())
                {
                    const bool allCovered = std::ranges::all_of(shotsAtQ, [&](const ShotPlan & sp) {
                        if (sp.threatIdx >= threats.size())
                            return false;
                        for (std::size_t fci = 0; fci < fcCount; ++fci)
                        {
                            if (fcDetersTh[fci * thCount + sp.threatIdx] != 0 &&
                                canFriendlyReachAttackOccupied(
                                    friendlyCover[fci].ki, friendlyCover[fci].pos, q,
                                    friendlyCover[fci].moveBudget, friendlyCover[fci].strikeRange,
                                    friendlyCover[fci].maxRange, occEnemyGen))
                                return true;
                        }
                        return false;
                    });
                    if (allCovered)
                    {
                        minEscapeLoss = 0;
                        bestUsedThreats.clear();
                        break;
                    }
                }
                if (!v.damaged && hasCombatUnitThreat && m_productionModel != nullptr)
                {
                    for (const TilePoint f : openEnemyFactories)
                    {
                        const auto counter = m_productionModel->bestEnemyCounterFromSite(f, v.ki);
                        if (!counter || counter->baseDamageVsFriendly <= 0.0)
                            continue;
                        const int cMove = counter->canMoveAndFire ? counter->movementPoints : 0;
                        std::uint8_t cAdjMask = 0;
                        if (counter->minRange == 1)
                        {
                            for (std::uint8_t dirIdx = 0; dirIdx < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++dirIdx)
                            {
                                const StepVector step = entryStep(ENTRY_DIRECTIONS[dirIdx]);
                                const TilePoint adj{q.x + step.dx, q.y + step.dy};
                                if (adj.x < 0 || adj.x >= m_local.width || adj.y < 0 || adj.y >= m_local.height)
                                    continue;
                                const std::size_t aIdx =
                                    static_cast<std::size_t>(adj.y) * static_cast<std::size_t>(m_local.width) +
                                    static_cast<std::size_t>(adj.x);
                                if (m_occEnemyGen[aIdx] == occEnemyGen)
                                    continue;
                                if (m_occFriendlyGen[aIdx] == occFriendlyGen && adj != ignoredFriendly)
                                    continue;
                                if (gridTileDistance(counter->gridId, f, adj, false) <= cMove)
                                    cAdjMask |= static_cast<std::uint8_t>(1u << dirIdx);
                            }
                        }
                        const bool reachesQ = counter->minRange == 1
                            ? cAdjMask != 0
                            : canSpawnReachAttack(counter->gridId, cMove, counter->minRange, counter->maxRange, f, q);
                        if (reachesQ)
                        {
                            shotsAtQ.push_back({std::numeric_limits<std::size_t>::max(),
                                                counter->baseDamageVsFriendly,
                                                static_cast<double>(UNIT_HP_STEPS),
                                                counter->friendlyReturnDamage, counter->minRange,
                                                counter->cost, NO_UNIT, cAdjMask});
                            break;
                        }
                    }
                }
                std::stable_sort(shotsAtQ.begin(), shotsAtQ.end(), [](const ShotPlan & a, const ShotPlan & b) {
                    const double sa = a.baseDmg * a.attackerHp;
                    const double sb = b.baseDmg * b.attackerHp;
                    if (sa != sb) return sa > sb;
                    return std::popcount(a.adjMask) < std::popcount(b.adjMask);
                });
                MilliFunds worstAtQ = 0;
                double remHp = v.hpAfterE0;
                MilliFunds combinedGain = 0;
                usedHere.clear();
                std::uint8_t usedAdjMask = 0;
                for (const auto & sp : shotsAtQ)
                {
                    if (remHp <= 0.0) break;
                    std::uint8_t chosenDirMask = 0;
                    if (sp.strikeRange == 1)
                    {
                        const std::uint8_t freeMask = static_cast<std::uint8_t>(sp.adjMask & ~usedAdjMask);
                        if (freeMask == 0)
                            continue;
                        for (std::uint8_t dirIdx = 0; dirIdx < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++dirIdx)
                        {
                            if ((freeMask & static_cast<std::uint8_t>(1u << dirIdx)) != 0)
                            {
                                chosenDirMask = static_cast<std::uint8_t>(1u << dirIdx);
                                break;
                            }
                        }
                    }
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
                        usedAdjMask |= chosenDirMask;
                        if (sp.threatIdx < threats.size())
                            usedHere.push_back(sp.threatIdx);
                        if (combinedGain > worstAtQ) worstAtQ = combinedGain;
                    }
                }
                if (worstAtQ < minEscapeLoss)
                {
                    minEscapeLoss = worstAtQ;
                    bestUsedThreats.assign(usedHere.begin(), usedHere.end());
                }
                if (minEscapeLoss == 0)
                    break;
            }
            if (minEscapeLoss > 0 && minEscapeLoss < std::numeric_limits<MilliFunds>::max())
            {
                for (const std::size_t ti : bestUsedThreats)
                    threatSpent[ti] = 1;
                RelativeStockDetail::checkedAdd(totalTrapLoss, minEscapeLoss);
            }
        }
        return totalTrapLoss;
    }

    MilliFunds UnifiedTeamEvaluator::computeEnemyDenial(const ReplyPassEvaluation & eval)
    {
        std::map<int, double> finalEnemyHp;
        for (const auto & actor : eval.actors)
        {
            if (actor.ownShot.target == NO_UNIT || isTerrainTargetId(actor.ownShot.target)) continue;
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
                const int dist = gridMoveCostForUnit(e, ep.tile, propTile);
                const int arriveTurns = (dist + knownE.movementPoints - 1) / knownE.movementPoints;
                const int capTurns = (capPts + knownE.captureRate - 1) / knownE.captureRate;
                bestReplaceFinish = std::min(bestReplaceFinish, arriveTurns + capTurns);
            }
            for (const TilePoint f : m_enemyFactories)
            {
                const int dist = m_sampleCapperKi != NO_UNIT
                    ? gridMoveCostForUnit(m_sampleCapperKi, f, propTile)
                    : (!m_local.grids.empty() ? gridTileDistance(0, f, propTile, false) : UNIFIED_UNREACHABLE_COST);
                if (dist >= UNIFIED_UNREACHABLE_COST)
                    continue;
                const int moveCap = m_sampleCapperKi != NO_UNIT
                    ? std::max(1, m_knowledge.units()[static_cast<std::size_t>(m_sampleCapperKi)].movementPoints)
                    : std::max(1, killedUnit.movementPoints);
                const int arriveTurns = 1 + (dist + moveCap - 1) / moveCap;
                const int capTurns = (capPts + UNIT_HP_STEPS - 1) / UNIT_HP_STEPS;
                bestReplaceFinish = std::min(bestReplaceFinish, arriveTurns + capTurns - 1);
            }
            if (bestReplaceFinish >= std::numeric_limits<int>::max()) return raw.delayCredit;
            const int delayTurns = std::max(0, std::min(horizon, bestReplaceFinish - beforeTurns));
            const auto col = mirroredColumn(m_stockField.enemyColumn(slot));
            const MilliFunds swing = incomeSwing(col.income, delayTurns, col.ownerBefore, OwnerSign::Enemy);
            const MilliFunds bounded = swing < 0 ? -swing : swing;
            return std::min(raw.delayCredit, bounded);
        };
        std::vector<std::int32_t> killedCapturingEnemies;
        std::vector<std::int32_t> killedRoamingCappers;
        MilliFunds woundedInterruptCredit = 0;
        MilliFunds killedInterruptFloor = 0;
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
                    if (capturing)
                        killedCapturingEnemies.push_back(targetKi);
                    else
                        killedRoamingCappers.push_back(targetKi);
                }
                RelativeStockDetail::checkedAdd(
                    killedInterruptFloor, boundedKillInterruptCredit(targetKi, interrupt));
                // Also mirror strategic capture denial when killing an enemy capturing a Com Tower, Power Plant, or production site.
                if (capturing && m_productionModel != nullptr && m_productionModel->valid())
                {
                    const auto prop = std::ranges::find_if(m_properties, [&](const PropertyFacts & p) {
                        return p.capturable && p.capturerIndex == targetKi && p.capturePoints > 0;
                    });
                    if (prop != m_properties.end())
                    {
                        const int delayTurns = std::max(1, (capPts + std::max(1, rateE) - 1) / std::max(1, rateE));
                        RelativeStockDetail::checkedAdd(
                            killedInterruptFloor,
                            m_productionModel->enemyCaptureInterruptStrategicValue({prop->x, prop->y}, delayTurns, horizon));
                    }
                }
            }
            else
            {
                if (interrupt.delayCredit > 0)
                    RelativeStockDetail::checkedAdd(woundedInterruptCredit, interrupt.delayCredit);
                if (m_productionModel != nullptr && m_productionModel->valid() &&
                    targetKi >= 0 && targetKi < static_cast<int>(m_knowledge.units().size()))
                {
                    const auto & ku = m_knowledge.units()[static_cast<std::size_t>(targetKi)];
                    if (ku.capturePoints > 0 && ku.captureRate > 0)
                    {
                        const auto prop = std::ranges::find_if(m_properties, [&](const PropertyFacts & p) {
                            return p.capturable && p.capturerIndex == targetKi && p.capturePoints > 0;
                        });
                        if (prop != m_properties.end())
                        {
                            const int beforeTurns = static_cast<int>(widenedCaptureTurns(prop->capturePoints, ku.captureRate, capPts));
                            const int afterSteps = ceilHpSteps(hpAfter);
                            const int afterRate = replyCaptureRate(
                                ku.captureRate, std::max(0, ku.hpRounded - afterSteps), afterSteps <= 0);
                            const int afterTurns = afterRate > 0
                                ? static_cast<int>(widenedCaptureTurns(prop->capturePoints, afterRate, capPts))
                                : horizon;
                            const int delayTurns = std::max(0, afterTurns - beforeTurns);
                            if (delayTurns > 0)
                            {
                                RelativeStockDetail::checkedAdd(
                                    woundedInterruptCredit,
                                    m_productionModel->enemyCaptureInterruptStrategicValue(
                                        {prop->x, prop->y}, delayTurns, horizon));
                            }
                        }
                    }
                }
            }
        }
        // Active capturing enemies use exact stock loss; non-capturing enemies use exact neutral-race delay only.
        MilliFunds killedStockLoss = m_stockField.enemyStockLoss(killedCapturingEnemies);
        MilliFunds roamingDenialCredit = 0;
        if (!killedRoamingCappers.empty() && !m_unownedNeutralProperties.empty())
        {
            std::vector<TilePoint> claimedProps;
            for (const std::int32_t killedKi : killedRoamingCappers)
            {
                const auto & ku = m_knowledge.units()[static_cast<std::size_t>(killedKi)];
                const int moveE = std::max(1, ku.movementPoints);
                TilePoint bestProp = INVALID_TILE;
                MilliFunds bestCredit = 0;
                for (const TilePoint propTile : m_unownedNeutralProperties)
                {
                    if (std::ranges::find(claimedProps, propTile) != claimedProps.end())
                        continue;
                    const int killDist = gridMoveCostForUnit(killedKi, {ku.x, ku.y}, propTile);
                    if (killDist >= UNIFIED_UNREACHABLE_COST)
                        continue;
                    const int killArrive = (killDist + moveE - 1) / moveE;
                    if (killArrive >= horizon)
                        continue;
                    int replaceArrive = std::numeric_limits<int>::max();
                    for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
                    {
                        if (e == killedKi || isKilledInF0(e) ||
                            std::ranges::find(killedRoamingCappers, e) != killedRoamingCappers.end())
                            continue;
                        const auto & ep = m_local.units[static_cast<std::size_t>(e)];
                        if (!ep.present || !ep.enemy || !ep.capper) continue;
                        const int eMove = std::max(1, ep.movement);
                        const int dist = gridMoveCostForUnit(e, ep.tile, propTile);
                        if (dist < UNIFIED_UNREACHABLE_COST)
                            replaceArrive = std::min(replaceArrive, (dist + eMove - 1) / eMove);
                    }
                    for (const TilePoint f : m_enemyFactories)
                    {
                        const int dist = gridMoveCostForUnit(killedKi, f, propTile);
                        if (dist < UNIFIED_UNREACHABLE_COST)
                            replaceArrive = std::min(replaceArrive, 1 + (dist + moveE - 1) / moveE);
                    }
                    const int delay = replaceArrive >= std::numeric_limits<int>::max()
                        ? (horizon - killArrive)
                        : std::max(0, std::min(horizon, replaceArrive) - killArrive);
                    const MilliFunds propCredit = static_cast<MilliFunds>(delay) * propertyOneTurnSwingAt(propTile);
                    if (propCredit > bestCredit)
                    {
                        bestCredit = propCredit;
                        bestProp = propTile;
                    }
                }
                if (bestCredit > 0 && bestProp != INVALID_TILE)
                {
                    claimedProps.push_back(bestProp);
                    RelativeStockDetail::checkedAdd(roamingDenialCredit, bestCredit);
                }
            }
        }
        MilliFunds total = std::max(killedStockLoss, killedInterruptFloor);
        RelativeStockDetail::checkedAdd(total, roamingDenialCredit);
        RelativeStockDetail::checkedAdd(total, woundedInterruptCredit);
        // Credit exact capture delay when an active enemy capper shoots instead of capturing on E0.
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
            const MilliFunds shotNet = shot.targetCapital - shot.counterCapital;
            MilliFunds capperInterruptCredit = 0;
            if (shotPos != ep.tile)
            {
                const auto abandoned = openRefinementCaptureInterruptCredit(
                    m_knowledge, m_stockField, m_properties, e, 0.0);
                RelativeStockDetail::checkedAdd(capperInterruptCredit, boundedKillInterruptCredit(e, abandoned));
            }
            else
            {
                RelativeStockDetail::checkedAdd(capperInterruptCredit, propertyOneTurnSwingAt(ep.tile));
                const auto wounded = openRefinementCaptureInterruptCredit(
                    m_knowledge, m_stockField, m_properties, e, afterHp);
                RelativeStockDetail::checkedAdd(capperInterruptCredit, wounded.delayCredit);
            }
            if (shotNet > capperInterruptCredit)
                RelativeStockDetail::checkedAdd(total, capperInterruptCredit);
        }
        return scalePaybackToRelevanceHorizon(total, horizon);
    }

    std::pair<int, int> UnifiedTeamEvaluator::unitAttackRangeUncached(int ki) const
    {
        if (ki >= 0 && ki < static_cast<int>(m_knowledge.units().size()))
        {
            const auto & known = m_knowledge.units()[static_cast<std::size_t>(ki)];
            return {std::max(1, known.minRange), std::max(1, known.maxRange)};
        }
        return {1, 1};
    }

    std::pair<int, int> UnifiedTeamEvaluator::unitAttackRange(int ki) const
    {
        if (ki >= 0 && ki < static_cast<int>(m_unitRanges.size()))
            return m_unitRanges[static_cast<std::size_t>(ki)];
        return unitAttackRangeUncached(ki);
    }

    bool UnifiedTeamEvaluator::tileEnterable(const MobilityCostGrid & grid, TilePoint t)
    {
        if (!grid.onGrid(t.x, t.y)) return false;
        for (const EntryDirection dir : ENTRY_DIRECTIONS)
        {
            if (grid.cost(t.x, t.y, dir) != IMPASSABLE_COST)
                return true;
        }
        return false;
    }

    std::vector<std::int32_t> UnifiedTeamEvaluator::buildReversePathDistancesOnGrid(
        const MobilityCostGrid & grid, std::span<const TilePoint> targets, int minRange, int maxRange) const
    {
        const std::size_t cellCount = static_cast<std::size_t>(m_local.width) * static_cast<std::size_t>(m_local.height);
        std::vector<std::int32_t> dist(cellCount, std::numeric_limits<std::int32_t>::max());
        if (targets.empty() || m_local.width <= 0 || m_local.height <= 0)
            return dist;
        const auto idxOf = [&](TilePoint t) {
            return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) + static_cast<std::size_t>(t.x);
        };
        using Node = std::pair<std::int32_t, TilePoint>;
        const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
        std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
        for (const TilePoint t : targets)
        {
            if (!grid.onGrid(t.x, t.y)) continue;
            if (minRange == 0)
            {
                if (dist[idxOf(t)] > 0)
                {
                    dist[idxOf(t)] = 0;
                    pq.push({0, t});
                }
            }
            if (maxRange >= 1)
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

    std::vector<std::int32_t> UnifiedTeamEvaluator::buildReversePathDistances(
        int gridId, std::span<const TilePoint> targets, int minRange, int maxRange) const
    {
        if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()))
            return std::vector<std::int32_t>(
                static_cast<std::size_t>(std::max(0, m_local.width)) * static_cast<std::size_t>(std::max(0, m_local.height)),
                std::numeric_limits<std::int32_t>::max());
        return buildReversePathDistancesOnGrid(m_local.grids[static_cast<std::size_t>(gridId)], targets, minRange, maxRange);
    }

    int UnifiedTeamEvaluator::cachedSetPathDistance(const std::map<int, std::vector<std::int32_t>> & table,
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
        (void)fallbackTargets;
        return UNIFIED_UNREACHABLE_COST;
    }

    bool UnifiedTeamEvaluator::canUnitReachAttack(int ki, TilePoint from, TilePoint target, int moveBudget,
                            int minRange, int maxRange) const
    {
        const int effMin = std::max(1, minRange);
        const int effMax = std::max(effMin, maxRange);
        const int manh = manhattanDistance(from, target);
        if (manh >= effMin && manh <= effMax) return true;
        if (moveBudget <= 0) return false;
        const int gridId = (ki >= 0 && ki < static_cast<int>(m_local.units.size()))
            ? pawnTravelGrid(m_local.units[static_cast<std::size_t>(ki)]) : -1;
        const int effGrid = (gridId >= 0 && gridId < static_cast<int>(m_local.grids.size()))
            ? gridId : (!m_local.grids.empty() ? 0 : -1);
        if (effGrid < 0 || m_local.width <= 0 || m_local.height <= 0 ||
            from.x < 0 || from.x >= m_local.width || from.y < 0 || from.y >= m_local.height ||
            target.x < 0 || target.x >= m_local.width || target.y < 0 || target.y >= m_local.height)
            return false;
        const PathDistKey key{effGrid, target.x, target.y, effMin, effMax};
        auto it = m_indirectTilePathCache.find(key);
        if (it == m_indirectTilePathCache.end())
        {
            it = m_indirectTilePathCache.emplace(
                key, buildReversePathDistances(effGrid, std::span<const TilePoint>(&target, 1), effMin, effMax)).first;
        }
        const std::size_t idx = static_cast<std::size_t>(from.y) * static_cast<std::size_t>(m_local.width) +
            static_cast<std::size_t>(from.x);
        return idx < it->second.size() && it->second[idx] <= moveBudget;
    }

    void UnifiedTeamEvaluator::computeBreakableProfiles()
    {
        m_breakableProfiles.clear();
        m_valuableBreakables.clear();
        if (m_local.breakableTerrains.empty() || m_local.width <= 0 || m_local.height <= 0 || m_local.grids.empty())
            return;
        const int horizon = m_stockField.horizonTurns() > 0
            ? m_stockField.horizonTurns() : DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
        const int capPts = m_stockField.capturePointsToCapture() > 0
            ? m_stockField.capturePointsToCapture() : UNIFIED_PROD_DEFAULT_CAP_POINTS;
        const int width = m_local.width;
        const auto distAt = [width](const std::vector<std::int32_t> & d, TilePoint p) -> int {
            if (p.x < 0 || p.y < 0 || p.x >= width)
                return std::numeric_limits<std::int32_t>::max();
            const std::size_t idx = static_cast<std::size_t>(p.y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(p.x);
            return idx < d.size() ? d[idx] : std::numeric_limits<std::int32_t>::max();
        };

        struct CapperCap
        {
            int gridId{-1};
            TilePoint origin{INVALID_TILE};
            int move{1};
            int capTurns{1};
        };
        std::vector<CapperCap> cappers;
        struct AttackerCap
        {
            int ki{NO_UNIT};
            int gridId{-1};
            TilePoint origin{INVALID_TILE};
            int move{1};
            int minR{1};
            int maxR{1};
            ProductionUnitTemplate tmpl{};
            double hp{static_cast<double>(UNIT_HP_STEPS)};
        };
        std::vector<AttackerCap> attackers;
        const bool hasWeaponModel = static_cast<bool>(m_local.weapon || m_local.weaponSlots);
        for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
        {
            const auto & pawn = m_local.units[static_cast<std::size_t>(ki)];
            if (!pawn.present || pawn.enemy || pawn.tile == INVALID_TILE) continue;
            const int gridId = pawnTravelGrid(pawn);
            if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size())) continue;
            const int move = std::max(1, pawn.movement);
            if (pawn.capper)
            {
                const int capRate = ki < static_cast<int>(m_knowledge.units().size())
                    ? std::max(1, m_knowledge.units()[static_cast<std::size_t>(ki)].captureRate)
                    : UNIT_HP_STEPS;
                cappers.push_back({gridId, pawn.tile, move, std::max(0, (capPts + capRate - 1) / capRate - 1)});
            }
            else if (hasWeaponModel)
            {
                const auto [minR, maxR] = unitAttackRange(ki);
                const double hp = pawn.realHp >= 0.0 ? pawn.realHp
                    : (ki < static_cast<int>(m_knowledge.units().size())
                           ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded)
                           : static_cast<double>(UNIT_HP_STEPS));
                if (hp <= 0.0) continue;
                ProductionUnitTemplate tmpl;
                tmpl.cost = pawn.replacementCost;
                tmpl.canMoveAndFire = minR <= 1;
                tmpl.minRange = minR;
                tmpl.maxRange = maxR;
                attackers.push_back({ki, gridId, pawn.tile, move, minR, maxR, tmpl, hp});
            }
        }

        struct UnownedPropInfo
        {
            TilePoint tile{INVALID_TILE};
            PropertyIncome income{};
            OwnerSign ownerBefore{OwnerSign::Neutral};
        };
        std::vector<UnownedPropInfo> propInfos;
        for (const auto & prop : m_properties)
        {
            const Relation rel = m_knowledge.relation(prop.ownerId);
            const bool isEnemyProp = prop.ownerId != NO_OWNER &&
                (m_knowledge.observerId() == NO_OWNER ? true : rel == Relation::Enemy);
            if (!prop.capturable || (prop.ownerId != NO_OWNER && !isEnemyProp))
                continue;
            PropertyIncome inc = captureIncome(prop);
            OwnerSign ownerBefore = prop.ownerId == NO_OWNER ? OwnerSign::Neutral : OwnerSign::Enemy;
            if (const int colSlot = m_stockField.columnSlotAt({prop.x, prop.y}); colSlot != NO_STOCK_COLUMN)
            {
                const auto col = mirroredColumn(m_stockField.enemyColumn(colSlot));
                inc = col.income;
                ownerBefore = col.ownerBefore;
            }
            propInfos.push_back({{prop.x, prop.y}, inc, ownerBefore});
        }
        if (m_properties.empty())
        {
            for (const TilePoint p : m_unownedProperties)
                propInfos.push_back({p, PropertyIncome{}, OwnerSign::Neutral});
        }

        std::map<int, int> maxDmgByTarget;
        for (const auto & member : m_problem.actors())
        {
            for (const auto & cand : member.candidates)
            {
                if (!cand.plannable || !isTerrainTargetId(cand.targetUnitId)) continue;
                if (const auto * comp = singleComponentOf(cand.source.bundle);
                    comp != nullptr && comp->kind == ComponentKind::Fire)
                {
                    auto & best = maxDmgByTarget[cand.targetUnitId];
                    best = std::max(best, comp->fire.damageSteps);
                }
            }
        }

        for (const auto & bt : m_local.breakableTerrains)
        {
            pumpUiEventsIfDue();
            const int targetId = encodeTerrainTargetId(bt.tile);
            BreakableProfile prof;
            prof.targetId = targetId;
            prof.tile = bt.tile;
            prof.initialHpSteps = std::max<std::int32_t>(1, bt.hpSteps);
            int maxSingle = maxDmgByTarget.contains(targetId) ? maxDmgByTarget[targetId] : 0;
            if (maxSingle <= 0 && m_local.envWeapon)
            {
                for (const auto & att : attackers)
                {
                    const double raw = m_local.envWeapon(att.ki, bt.tile);
                    if (raw > 0.0)
                        maxSingle = std::max(maxSingle, damageStepsFrom(static_cast<double>(prof.initialHpSteps), raw));
                }
            }
            prof.maxSingleTurnSteps = std::max(1, maxSingle);
            prof.surplusByBreachDelay.assign(static_cast<std::size_t>(horizon + 2), 0);

            std::map<int, MobilityCostGrid> openGrids;
            const auto getOpenGrid = [&](int gridId) -> const MobilityCostGrid & {
                auto [it, ins] = openGrids.try_emplace(gridId);
                if (ins)
                {
                    it->second = m_local.grids[static_cast<std::size_t>(gridId)];
                    for (const TilePoint o : bt.openedTiles)
                        if (it->second.onGrid(o.x, o.y))
                            it->second.setTileCost(o.x, o.y, 1);
                }
                return it->second;
            };

            for (const auto & pInfo : propInfos)
            {
                int closedBestTurns = horizon + 1;
                int openBestTurns = horizon + 1;
                std::map<int, std::vector<std::int32_t>> closedCache;
                std::map<int, std::vector<std::int32_t>> openCache;
                for (const auto & cap : cappers)
                {
                    auto & cField = closedCache[cap.gridId];
                    if (cField.empty())
                        cField = buildReversePathDistances(cap.gridId, std::span<const TilePoint>(&pInfo.tile, 1), 0, 0);
                    const int cd = distAt(cField, cap.origin);
                    if (cd < std::numeric_limits<std::int32_t>::max())
                        closedBestTurns = std::min(closedBestTurns, (cd + cap.move - 1) / cap.move + cap.capTurns);

                    auto & oField = openCache[cap.gridId];
                    if (oField.empty())
                        oField = buildReversePathDistancesOnGrid(
                            getOpenGrid(cap.gridId), std::span<const TilePoint>(&pInfo.tile, 1), 0, 0);
                    const int od = distAt(oField, cap.origin);
                    if (od < std::numeric_limits<std::int32_t>::max())
                        openBestTurns = std::min(openBestTurns, (od + cap.move - 1) / cap.move + cap.capTurns);
                }
                if (openBestTurns < closedBestTurns)
                {
                    const MilliFunds closedGain = closedBestTurns < horizon
                        ? incomeSwing(pInfo.income, horizon - closedBestTurns, pInfo.ownerBefore, OwnerSign::Ours)
                        : 0;
                    for (int d = 0; d <= horizon; ++d)
                    {
                        const int turnsAtD = d + openBestTurns;
                        if (turnsAtD < closedBestTurns && turnsAtD < horizon)
                        {
                            const MilliFunds openGain = incomeSwing(
                                pInfo.income, horizon - turnsAtD, pInfo.ownerBefore, OwnerSign::Ours);
                            if (openGain > closedGain)
                                RelativeStockDetail::checkedAdd(
                                    prof.surplusByBreachDelay[static_cast<std::size_t>(d)], openGain - closedGain);
                        }
                    }
                }
            }

            for (const auto & [eKi, eTile] : m_enemyUnits)
            {
                const auto & ep = m_local.units[static_cast<std::size_t>(eKi)];
                if (ep.replacementCost <= 0) continue;
                const double eHp = ep.realHp >= 0.0 ? ep.realHp
                    : (eKi < static_cast<int>(m_knowledge.units().size())
                           ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(eKi)].hpRounded)
                           : static_cast<double>(UNIT_HP_STEPS));
                const MilliFunds liveCap = static_cast<MilliFunds>(
                    std::llround((static_cast<double>(ep.replacementCost) * eHp) / UNIFIED_PROD_FULL_HP));
                if (liveCap <= 0) continue;
                const auto [eMinR, eMaxR] = unitAttackRange(eKi);
                ProductionEnemyUnit eu;
                eu.replacementCost = ep.replacementCost;
                eu.hp = eHp;
                eu.canMoveAndFire = eMinR <= 1;
                eu.minRange = eMinR;
                eu.maxRange = eMaxR;

                int closedEnemyTurns = horizon + 1;
                struct OpenAttackerTrade
                {
                    int openTurns{0};
                    MilliFunds netTrade{0};
                };
                std::vector<OpenAttackerTrade> unlockedTrades;
                for (const auto & att : attackers)
                {
                    const double baseDmg = maxWeaponBaseDamage(att.ki, eKi, att.minR);
                    if (baseDmg <= 0.0) continue;
                    const double scaledDmg = baseDmg * (att.hp / UNIFIED_PROD_FULL_HP);
                    const double retDmg = (att.tmpl.canMoveAndFire && eu.canMoveAndFire)
                        ? std::max(0.0, maxWeaponBaseDamage(eKi, att.ki, 1))
                        : 0.0;
                    const MilliFunds netTrade = static_cast<MilliFunds>(std::llround(
                        UnifiedProductionModel::firstStrikeNetTrade(att.tmpl, eu, scaledDmg, retDmg)));
                    if (netTrade <= 0) continue;
                    const auto closedField = buildReversePathDistances(
                        att.gridId, std::span<const TilePoint>(&eTile, 1), att.minR, att.maxR);
                    const int cd = distAt(closedField, att.origin);
                    if (cd < std::numeric_limits<std::int32_t>::max())
                        closedEnemyTurns = std::min(closedEnemyTurns, (cd + att.move - 1) / att.move);
                    const auto openField = buildReversePathDistancesOnGrid(
                        getOpenGrid(att.gridId), std::span<const TilePoint>(&eTile, 1), att.minR, att.maxR);
                    const int od = distAt(openField, att.origin);
                    if (od < std::numeric_limits<std::int32_t>::max())
                        unlockedTrades.push_back({(od + att.move - 1) / att.move, netTrade});
                }
                if (!unlockedTrades.empty())
                {
                    for (int d = 0; d <= horizon; ++d)
                    {
                        MilliFunds tradeSum = 0;
                        for (const auto & t : unlockedTrades)
                        {
                            const int arrival = d + t.openTurns;
                            if (arrival < closedEnemyTurns && arrival < horizon)
                                RelativeStockDetail::checkedAdd(tradeSum, t.netTrade);
                        }
                        if (tradeSum > 0)
                            RelativeStockDetail::checkedAdd(
                                prof.surplusByBreachDelay[static_cast<std::size_t>(d)], std::min(liveCap, tradeSum));
                    }
                }
            }

            if (bt.directWeaponStake > 0)
            {
                for (int d = 0; d < horizon; ++d)
                {
                    const MilliFunds timedWeaponStake =
                        (bt.directWeaponStake * (horizon - d)) / std::max(1, horizon);
                    RelativeStockDetail::checkedAdd(
                        prof.surplusByBreachDelay[static_cast<std::size_t>(d)], timedWeaponStake);
                }
            }
            if (!prof.surplusByBreachDelay.empty() && prof.surplusByBreachDelay.front() > 0 && bt.hpSteps > 0)
                m_valuableBreakables.push_back(bt.tile);
            if (bt.hpSteps > 0)
                m_breakableProfiles.emplace(targetId, std::move(prof));
        }
    }

    MilliFunds UnifiedTeamEvaluator::computeBreakableTerrainValue(const ReplyPassEvaluation & eval) const
    {
        if (m_breakableProfiles.empty() && m_local.breakableTerrains.empty())
            return 0;
        std::map<int, std::int32_t> dealtByTarget;
        for (const auto & actor : eval.actors)
        {
            if (!isTerrainTargetId(actor.ownShot.target)) continue;
            const std::int32_t dealt = std::max<std::int32_t>(
                0, ceilHpSteps(actor.ownShot.targetBefore) - ceilHpSteps(actor.ownShot.targetAfter));
            if (dealt > 0)
                dealtByTarget[actor.ownShot.target] += dealt;
        }
        const int horizon = m_stockField.horizonTurns() > 0
            ? m_stockField.horizonTurns() : DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
        MilliFunds total = 0;
        for (const auto & [targetId, dealt] : dealtByTarget)
        {
            const auto it = m_breakableProfiles.find(targetId);
            if (it == m_breakableProfiles.end() || it->second.surplusByBreachDelay.empty() ||
                it->second.surplusByBreachDelay.front() <= 0)
                continue;
            const auto & prof = it->second;
            const auto sampleSurplus = [&](std::int32_t steps) -> MilliFunds {
                if (steps <= 0)
                    return prof.surplusByBreachDelay.front();
                const double exactDelay = std::min(
                    static_cast<double>(horizon + 1),
                    static_cast<double>(steps) / static_cast<double>(prof.maxSingleTurnSteps));
                const int lo = static_cast<int>(std::floor(exactDelay));
                const int hi = std::min(horizon + 1, lo + 1);
                const double frac = exactDelay - static_cast<double>(lo);
                const double sLo = static_cast<double>(prof.surplusByBreachDelay[static_cast<std::size_t>(lo)]);
                const double sHi = static_cast<double>(prof.surplusByBreachDelay[static_cast<std::size_t>(hi)]);
                return static_cast<MilliFunds>(std::llround(sLo + (sHi - sLo) * frac));
            };
            const int remSteps = std::max(0, prof.initialHpSteps - dealt);
            const MilliFunds gain = sampleSurplus(remSteps) - sampleSurplus(prof.initialHpSteps);
            if (gain > 0)
                RelativeStockDetail::checkedAdd(total, gain);
        }
        // Price E0 weapon-building danger zones (Black Cannons, Mini Cannons, Lasers, Death Rays, Volcanoes) unless destroyed in F0.
        bool anyActiveHazard = false;
        for (const auto & bt : m_local.breakableTerrains)
        {
            if (bt.hazardActiveE0 && bt.hazardDamageSteps > 0 && !bt.hazardTiles.empty())
            {
                anyActiveHazard = true;
                break;
            }
        }
        if (anyActiveHazard)
        {
            std::vector<std::tuple<TilePoint, MilliFunds, std::int32_t>> survivingFriendlies;
            survivingFriendlies.reserve(eval.actors.size());
            std::vector<std::uint8_t> seenActor(m_local.units.size(), 0);
            for (const auto & actor : eval.actors)
            {
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                const int ki = member.knowledgeUnitIndex;
                if (ki >= 0 && ki < static_cast<int>(seenActor.size()))
                    seenActor[static_cast<std::size_t>(ki)] = 1;
                if (actor.stepsAfterReply <= 0) continue;
                const TilePoint dst = member.candidates[static_cast<std::size_t>(actor.candidateIndex)].destination;
                const MilliFunds cost = ki >= 0 && ki < static_cast<int>(m_local.units.size())
                    ? m_local.units[static_cast<std::size_t>(ki)].replacementCost : 0;
                if (cost > 0)
                    survivingFriendlies.emplace_back(dst, cost, actor.stepsAfterReply);
            }
            for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
            {
                if (seenActor[static_cast<std::size_t>(ki)] != 0) continue;
                const auto & p = m_local.units[static_cast<std::size_t>(ki)];
                if (!p.present || p.enemy || p.replacementCost <= 0 || p.tile == INVALID_TILE) continue;
                int steps = ki < static_cast<int>(m_knowledge.units().size())
                    ? m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded : UNIT_HP_STEPS;
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    steps = ceilHpSteps(it->second);
                if (steps > 0)
                    survivingFriendlies.emplace_back(p.tile, p.replacementCost, steps);
            }
            for (const auto & bt : m_local.breakableTerrains)
            {
                if (!bt.hazardActiveE0 || bt.hazardDamageSteps <= 0 || bt.hazardTiles.empty())
                    continue;
                if (bt.hpSteps > 0)
                {
                    const int tid = encodeTerrainTargetId(bt.tile);
                    if (const auto it = dealtByTarget.find(tid); it != dealtByTarget.end() && it->second >= bt.hpSteps)
                        continue;
                }
                MilliFunds singleWorst = 0;
                MilliFunds aoeSum = 0;
                for (const auto & [dst, cost, steps] : survivingFriendlies)
                {
                    if (std::ranges::find(bt.hazardTiles, dst) == bt.hazardTiles.end())
                        continue;
                    // Black Cannons/Lasers reduce unit HP down to 1 HP minimum (non-lethal engine cap).
                    const std::int32_t dealtSteps = std::min(bt.hazardDamageSteps, std::max<std::int32_t>(0, steps - 1));
                    const MilliFunds dmgVal = dealtSteps > 0 ? bookDamage(cost, dealtSteps) : 0;
                    if (bt.hazardSingleTarget)
                        singleWorst = std::max(singleWorst, dmgVal);
                    else
                        RelativeStockDetail::checkedAdd(aoeSum, dmgVal);
                }
                const MilliFunds hazardLoss = bt.hazardSingleTarget ? singleWorst : aoeSum;
                if (hazardLoss > 0)
                    RelativeStockDetail::checkedSubtract(total, hazardLoss);
            }
        }
        return total;
    }

    void UnifiedTeamEvaluator::buildPathDistances()
    {
        if (m_local.width <= 0 || m_local.height <= 0)
            return;

        const auto attackPathDistFromOrigin = [&](int gridId, TilePoint origin, TilePoint target, int minR, int maxR) -> int {
            if (origin == INVALID_TILE || gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()) ||
                origin.x < 0 || origin.x >= m_local.width || origin.y < 0 || origin.y >= m_local.height)
                return std::numeric_limits<std::int32_t>::max();
            const std::size_t idx = static_cast<std::size_t>(origin.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(origin.x);
            if (minR > 1)
            {
                const PathDistKey key{gridId, target.x, target.y, minR, maxR};
                auto it = m_indirectTilePathCache.find(key);
                if (it == m_indirectTilePathCache.end())
                    it = m_indirectTilePathCache.emplace(
                        key, buildReversePathDistances(gridId, std::span<const TilePoint>(&target, 1), minR, maxR)).first;
                return idx < it->second.size() ? it->second[idx] : std::numeric_limits<std::int32_t>::max();
            }
            const PathDistKey key{gridId, target.x, target.y, 1, 1};
            auto it = m_tilePathDistCache.find(key);
            if (it == m_tilePathDistCache.end())
                it = m_tilePathDistCache.emplace(
                    key, buildReversePathDistances(gridId, std::span<const TilePoint>(&target, 1), 1, 1)).first;
            return idx < it->second.size() ? it->second[idx] : std::numeric_limits<std::int32_t>::max();
        };

        const auto buildOpenPropGrid = [&](int gridId) {
            MobilityCostGrid openGrid = m_local.grids[static_cast<std::size_t>(gridId)];
            for (const auto & bt : m_local.breakableTerrains)
            {
                if (breakableTerrainStake(encodeTerrainTargetId(bt.tile)) <= 0) continue;
                for (const TilePoint o : bt.openedTiles)
                    if (openGrid.onGrid(o.x, o.y))
                        openGrid.setTileCost(o.x, o.y, 1);
            }
            return openGrid;
        };

        // Assign combat units to uncovered favorable enemy fronts using conserved action-economy coverage.
        struct FriendlyCombatRouter
        {
            int ki{NO_UNIT};
            int gridId{-1};
            int minR{1};
            int maxR{1};
            int move{1};
            double hp{static_cast<double>(UNIT_HP_STEPS)};
            MilliFunds cost{0};
            TilePoint origin{INVALID_TILE};
            std::vector<std::size_t> favorableEnemyIndices;
            std::vector<std::size_t> damageableEnemyIndices;
            std::vector<double> netTradeVsEnemy;
            int minFavDist{std::numeric_limits<int>::max()};
        };
        std::vector<FriendlyCombatRouter> combatRouters;
        const bool hasWeaponModel = static_cast<bool>(m_local.weapon || m_local.weaponSlots);

        m_repairPathDistByKi.assign(m_unitCount, nullptr);
        m_repairSiteMaskByKi.assign(m_unitCount, nullptr);
        for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
        {
            const auto & pawn = m_local.units[static_cast<std::size_t>(ki)];
            if (!pawn.present || pawn.enemy) continue;
            const int gridId = pawnTravelGrid(pawn);
            if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size())) continue;
            if (!m_friendlyFactories.empty() && !m_factoryPathDistByGrid.contains(gridId))
                m_factoryPathDistByGrid.emplace(gridId, buildReversePathDistances(gridId, m_friendlyFactories));
            if (!m_friendlyRepairSites.empty())
            {
                if (!m_repairPathDistByGrid.contains(gridId))
                    m_repairPathDistByGrid.emplace(gridId, buildReversePathDistances(gridId, m_friendlyRepairSites));
                std::vector<std::uint8_t> allowed(m_friendlyRepairSites.size(), 0);
                bool anyAllowed = false;
                for (std::size_t si = 0; si < m_friendlyRepairSites.size(); ++si)
                {
                    if (!m_local.canRepairOnTerrain || m_local.canRepairOnTerrain(ki, m_friendlyRepairSites[si]))
                    {
                        allowed[si] = 1;
                        anyAllowed = true;
                    }
                }
                if (anyAllowed)
                {
                    const auto maskIt = m_repairCellMaskCache.find(allowed);
                    const std::vector<std::uint8_t> * maskPtr = nullptr;
                    if (maskIt != m_repairCellMaskCache.end())
                    {
                        maskPtr = &maskIt->second;
                    }
                    else
                    {
                        std::vector<std::uint8_t> cellMask(m_cellCount, 0);
                        for (std::size_t si = 0; si < m_friendlyRepairSites.size(); ++si)
                        {
                            if (allowed[si] == 0) continue;
                            const TilePoint s = m_friendlyRepairSites[si];
                            if (s.x >= 0 && s.x < m_local.width && s.y >= 0 && s.y < m_local.height)
                                cellMask[static_cast<std::size_t>(s.y) * static_cast<std::size_t>(m_local.width) +
                                         static_cast<std::size_t>(s.x)] = 1;
                        }
                        maskPtr = &m_repairCellMaskCache.emplace(allowed, std::move(cellMask)).first->second;
                    }
                    m_repairSiteMaskByKi[static_cast<std::size_t>(ki)] = maskPtr;

                    const auto distKey = std::pair{gridId, allowed};
                    auto distIt = m_repairDistCache.find(distKey);
                    if (distIt == m_repairDistCache.end())
                    {
                        std::vector<TilePoint> allowedSites;
                        allowedSites.reserve(m_friendlyRepairSites.size());
                        for (std::size_t si = 0; si < m_friendlyRepairSites.size(); ++si)
                            if (allowed[si] != 0) allowedSites.push_back(m_friendlyRepairSites[si]);
                        distIt = m_repairDistCache.emplace(
                            std::move(distKey), buildReversePathDistances(gridId, allowedSites)).first;
                    }
                    m_repairPathDistByKi[static_cast<std::size_t>(ki)] = &distIt->second;
                }
            }
            if (!m_unownedProperties.empty() && !m_unownedPropPathDistByGrid.contains(gridId))
            {
                m_unownedPropPathDistByGrid.emplace(gridId, buildReversePathDistances(gridId, m_unownedProperties));
                if (!m_valuableBreakables.empty())
                    m_openPropPathDistByGrid.emplace(
                        gridId, buildReversePathDistancesOnGrid(buildOpenPropGrid(gridId), m_unownedProperties));
            }
            if (!m_unownedFactories.empty() && !m_unownedFactoryPathDistByGrid.contains(gridId))
                m_unownedFactoryPathDistByGrid.emplace(gridId, buildReversePathDistances(gridId, m_unownedFactories));
            if (!m_enemyOwnedProperties.empty() && !m_enemyPropPathDistByGrid.contains(gridId))
                m_enemyPropPathDistByGrid.emplace(gridId, buildReversePathDistances(gridId, m_enemyOwnedProperties));

            const bool unitHasWeapons = ki < static_cast<int>(m_knowledge.units().size())
                ? m_knowledge.units()[static_cast<std::size_t>(ki)].hasWeapons
                : hasWeaponModel;
            if (!unitHasWeapons && !pawn.capper && (pawn.supportsUnits || pawn.transportCapacity))
            {
                constexpr int UNIFIED_RESUPPLY_TARGET_TAG_BASE = -1000;
                constexpr int UNIFIED_SUPPORT_ESCORT_TAG_BASE = -1500;
                constexpr int UNIFIED_SUPPORT_PROP_TAG_BASE = -2000;
                std::vector<TilePoint> targetTiles;
                std::vector<int> targetIds;
                int supMinR = 0;
                int supMaxR = 0;
                if (pawn.supportsUnits)
                {
                    supMinR = 1;
                    supMaxR = 1;
                    for (int fKi = 0; fKi < static_cast<int>(m_local.units.size()); ++fKi)
                    {
                        if (fKi == ki) continue;
                        const auto & ally = m_local.units[static_cast<std::size_t>(fKi)];
                        if (!ally.present || ally.enemy || ally.tile == INVALID_TILE) continue;
                        if (ally.hasNativeWeapon && ally.ammoByWeapon[0] == 0)
                        {
                            targetTiles.push_back(ally.tile);
                            targetIds.push_back(UNIFIED_RESUPPLY_TARGET_TAG_BASE - fKi);
                        }
                    }
                    if (targetTiles.empty())
                    {
                        for (int fKi = 0; fKi < static_cast<int>(m_local.units.size()); ++fKi)
                        {
                            if (fKi == ki) continue;
                            const auto & ally = m_local.units[static_cast<std::size_t>(fKi)];
                            if (!ally.present || ally.enemy || ally.tile == INVALID_TILE || ally.ignoresCollision) continue;
                            const bool allyArmed = fKi < static_cast<int>(m_knowledge.units().size())
                                ? m_knowledge.units()[static_cast<std::size_t>(fKi)].hasWeapons
                                : ally.hasNativeWeapon;
                            if (allyArmed && !ally.capper)
                            {
                                targetTiles.push_back(ally.tile);
                                targetIds.push_back(UNIFIED_SUPPORT_ESCORT_TAG_BASE - fKi);
                            }
                        }
                    }
                    if (targetTiles.empty())
                    {
                        for (int fKi = 0; fKi < static_cast<int>(m_local.units.size()); ++fKi)
                        {
                            if (fKi == ki) continue;
                            const auto & ally = m_local.units[static_cast<std::size_t>(fKi)];
                            if (!ally.present || ally.enemy || ally.tile == INVALID_TILE) continue;
                            targetTiles.push_back(ally.tile);
                            targetIds.push_back(UNIFIED_SUPPORT_ESCORT_TAG_BASE - fKi);
                        }
                    }
                }
                if (targetTiles.empty())
                {
                    supMinR = 0;
                    supMaxR = 0;
                    if (!m_unownedProperties.empty())
                    {
                        targetTiles = m_unownedProperties;
                        targetIds.push_back(UNIFIED_SUPPORT_PROP_TAG_BASE - gridId);
                    }
                    else if (!m_enemyOwnedProperties.empty())
                    {
                        targetTiles = m_enemyOwnedProperties;
                        targetIds.push_back(UNIFIED_SUPPORT_PROP_TAG_BASE - gridId);
                    }
                }
                if (!targetTiles.empty())
                {
                    const auto profKey = std::tuple{gridId, supMinR, supMaxR, targetIds};
                    auto [it, inserted] = m_enemyPathDistByTargetSet.emplace(profKey, std::vector<std::int32_t>{});
                    if (inserted)
                        it->second = buildReversePathDistances(gridId, targetTiles, supMinR, supMaxR);
                    m_enemyPathDistByKi.emplace(ki, &it->second);
                }
                continue;
            }

            if (m_enemyPositions.empty() && m_valuableBreakables.empty()) continue;
            const auto [minR, maxR] = unitAttackRange(ki);
            if (!hasWeaponModel || pawn.capper)
            {
                std::vector<TilePoint> targetTiles;
                std::vector<int> targetIds;
                if (hasWeaponModel)
                {
                    for (const auto & [eKi, eTile] : m_enemyUnits)
                    {
                        if (maxWeaponBaseDamage(ki, eKi, minR) > 0.0)
                        {
                            targetTiles.push_back(eTile);
                            targetIds.push_back(eKi);
                        }
                    }
                }
                const std::span<const TilePoint> effectiveTargets = targetTiles.empty()
                    ? std::span<const TilePoint>(m_enemyPositions)
                    : std::span<const TilePoint>(targetTiles);
                if (!targetTiles.empty())
                    m_targetableEnemiesByKi.emplace(ki, std::move(targetTiles));
                if (!effectiveTargets.empty())
                {
                    const auto profKey = std::tuple{gridId, minR, maxR, targetIds};
                    auto [it, inserted] = m_enemyPathDistByTargetSet.emplace(profKey, std::vector<std::int32_t>{});
                    if (inserted)
                        it->second = buildReversePathDistances(gridId, effectiveTargets, minR, maxR);
                    m_enemyPathDistByKi.emplace(ki, &it->second);
                }
                continue;
            }

            FriendlyCombatRouter router;
            router.ki = ki;
            router.gridId = gridId;
            router.minR = minR;
            router.maxR = maxR;
            router.move = std::max(1, pawn.movement);
            router.hp = pawn.realHp >= 0.0 ? pawn.realHp
                : (ki < static_cast<int>(m_knowledge.units().size())
                       ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded)
                       : static_cast<double>(UNIT_HP_STEPS));
            router.cost = pawn.replacementCost;
            router.origin = pawn.tile;
            router.netTradeVsEnemy.assign(m_enemyUnits.size(), 0.0);

            std::vector<TilePoint> allDamageableTiles;
            ProductionUnitTemplate fTmpl;
            fTmpl.cost = router.cost;
            fTmpl.canMoveAndFire = minR <= 1;
            fTmpl.minRange = minR;
            fTmpl.maxRange = maxR;

            for (std::size_t ei = 0; ei < m_enemyUnits.size(); ++ei)
            {
                const auto [eKi, eTile] = m_enemyUnits[ei];
                const double baseDmg = maxWeaponBaseDamage(ki, eKi, minR);
                if (baseDmg <= 0.0) continue;
                allDamageableTiles.push_back(eTile);
                router.damageableEnemyIndices.push_back(ei);
                const auto & ep = m_local.units[static_cast<std::size_t>(eKi)];
                const auto [eMinR, eMaxR] = unitAttackRange(eKi);
                ProductionEnemyUnit eu;
                eu.replacementCost = ep.replacementCost;
                eu.hp = ep.realHp >= 0.0 ? ep.realHp
                    : (eKi < static_cast<int>(m_knowledge.units().size())
                           ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(eKi)].hpRounded)
                           : static_cast<double>(UNIT_HP_STEPS));
                eu.canMoveAndFire = eMinR <= 1;
                eu.minRange = eMinR;
                eu.maxRange = eMaxR;
                const double scaledDmg = baseDmg * (router.hp / UNIFIED_PROD_FULL_HP);
                const double retDmg = (fTmpl.canMoveAndFire && eu.canMoveAndFire)
                    ? std::max(0.0, maxWeaponBaseDamage(eKi, ki, 1))
                    : 0.0;
                const double netTrade = UnifiedProductionModel::firstStrikeNetTrade(fTmpl, eu, scaledDmg, retDmg);
                router.netTradeVsEnemy[ei] = netTrade;
                const double eStrike = std::max(0.0, maxWeaponBaseDamage(eKi, ki, eMinR));
                const double fCounter = (fTmpl.canMoveAndFire && eu.canMoveAndFire)
                    ? std::max(0.0, maxWeaponBaseDamage(ki, eKi, 1))
                    : 0.0;
                const double threat = UnifiedProductionModel::enemyFirstStrikeNetThreat(fTmpl, eu, eStrike, fCounter);
                if (netTrade > 0.0 && (netTrade >= threat || minR > 1))
                {
                    router.favorableEnemyIndices.push_back(ei);
                    if (router.origin != INVALID_TILE)
                    {
                        const int pd = attackPathDistFromOrigin(
                            router.gridId, router.origin, eTile, router.minR, router.maxR);
                        router.minFavDist = std::min(router.minFavDist, pd);
                    }
                }
            }
            for (const TilePoint btTile : m_valuableBreakables)
            {
                if (!m_local.envWeapon || m_local.envWeapon(ki, btTile) > 0.0)
                    allDamageableTiles.push_back(btTile);
            }
            if (!allDamageableTiles.empty())
                m_targetableEnemiesByKi.emplace(ki, std::move(allDamageableTiles));
            combatRouters.push_back(std::move(router));
        }

        // Route specialized units first (fewest favorable targets), then nearest favorable target.
        std::stable_sort(combatRouters.begin(), combatRouters.end(),
                         [](const FriendlyCombatRouter & a, const FriendlyCombatRouter & b) {
            const std::size_t aFav = a.favorableEnemyIndices.empty()
                ? a.damageableEnemyIndices.size() : a.favorableEnemyIndices.size();
            const std::size_t bFav = b.favorableEnemyIndices.empty()
                ? b.damageableEnemyIndices.size() : b.favorableEnemyIndices.size();
            return std::tuple{aFav, a.minFavDist, a.ki} < std::tuple{bFav, b.minFavDist, b.ki};
        });

        const int horizon = m_stockField.horizonTurns() > 0
            ? m_stockField.horizonTurns() : DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
        std::vector<double> frontCoverage(m_enemyUnits.size(), 0.0);
        for (const auto & router : combatRouters)
        {
            const auto & pool = !router.favorableEnemyIndices.empty()
                ? router.favorableEnemyIndices : router.damageableEnemyIndices;
            std::vector<std::pair<std::size_t, int>> reachablePool;
            for (const std::size_t ei : pool)
            {
                const int cd = attackPathDistFromOrigin(
                    router.gridId, router.origin, m_enemyUnits[ei].second, router.minR, router.maxR);
                if (cd < std::numeric_limits<std::int32_t>::max())
                    reachablePool.emplace_back(ei, cd);
            }

            std::vector<TilePoint> assignedTiles;
            std::vector<int> assignedIds;
            bool useOpenGrid = false;
            if (!reachablePool.empty())
            {
                std::size_t bestEi = reachablePool.front().first;
                double bestPriority = -std::numeric_limits<double>::infinity();
                const bool hasUncoveredInPool = std::ranges::any_of(reachablePool, [&](const auto & pair) {
                    return std::max(0.0, 1.0 - std::max(0.0, frontCoverage[pair.first])) > 0.0;
                });
                for (const auto & [ei, closedDist] : reachablePool)
                {
                    const auto [eKi, eTile] = m_enemyUnits[ei];
                    const double remaining = hasUncoveredInPool
                        ? std::max(0.0, 1.0 - std::max(0.0, frontCoverage[ei]))
                        : 1.0;
                    if (remaining <= 0.0)
                        continue;
                    const int arriveTurns = (closedDist + router.move - 1) / router.move;
                    const double trade = router.netTradeVsEnemy[ei] > 0.0
                        ? router.netTradeVsEnemy[ei]
                        : static_cast<double>(std::max<MilliFunds>(1, m_local.units[static_cast<std::size_t>(eKi)].replacementCost));
                    const double inHorizonFactor = arriveTurns < horizon
                        ? static_cast<double>(horizon - arriveTurns) / static_cast<double>(horizon)
                        : 0.0;
                    const double prio = inHorizonFactor > 0.0
                        ? (trade * remaining * inHorizonFactor)
                        : -static_cast<double>(closedDist);
                    if (prio > bestPriority)
                    {
                        bestPriority = prio;
                        bestEi = ei;
                    }
                }
                const TilePoint anchorTile = m_enemyUnits[bestEi].second;
                const int clusterReach = router.move + router.maxR;
                for (const auto & [ei, closedDist] : reachablePool)
                {
                    (void)closedDist;
                    const auto [eKi, eTile] = m_enemyUnits[ei];
                    if (ei == bestEi || gridTileDistance(router.gridId, anchorTile, eTile, false) <= clusterReach)
                    {
                        assignedTiles.push_back(eTile);
                        assignedIds.push_back(eKi);
                    }
                }
                const int bestEKi = m_enemyUnits[bestEi].first;
                const auto & bestEp = m_local.units[static_cast<std::size_t>(bestEKi)];
                const double bestHp = bestEp.realHp >= 0.0 ? bestEp.realHp
                    : (bestEKi < static_cast<int>(m_knowledge.units().size())
                           ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(bestEKi)].hpRounded)
                           : static_cast<double>(UNIT_HP_STEPS));
                const double liveCap = (static_cast<double>(bestEp.replacementCost) * bestHp) / UNIFIED_PROD_FULL_HP;
                const double deltaCov = liveCap > 0.0 && router.netTradeVsEnemy[bestEi] > 0.0
                    ? std::min(1.0, router.netTradeVsEnemy[bestEi] / liveCap)
                    : 1.0;
                frontCoverage[bestEi] += deltaCov;
            }
            else if (!m_valuableBreakables.empty())
            {
                for (const TilePoint btTile : m_valuableBreakables)
                {
                    if (m_local.envWeapon && m_local.envWeapon(router.ki, btTile) <= 0.0)
                        continue;
                    const int bd = attackPathDistFromOrigin(
                        router.gridId, router.origin, btTile, router.minR, router.maxR);
                    if (bd < std::numeric_limits<std::int32_t>::max())
                    {
                        assignedTiles.push_back(btTile);
                        assignedIds.push_back(encodeTerrainTargetId(btTile));
                    }
                }
                if (assignedTiles.empty() && !m_enemyPositions.empty())
                {
                    useOpenGrid = true;
                    assignedIds.push_back(-2 - router.gridId);
                }
            }
            const std::span<const TilePoint> effectiveTargets = assignedTiles.empty()
                ? std::span<const TilePoint>(m_enemyPositions)
                : std::span<const TilePoint>(assignedTiles);
            if (!effectiveTargets.empty())
            {
                const auto profKey = std::tuple{router.gridId, router.minR, router.maxR, assignedIds};
                auto [it, inserted] = m_enemyPathDistByTargetSet.emplace(profKey, std::vector<std::int32_t>{});
                if (inserted)
                {
                    it->second = useOpenGrid
                        ? buildReversePathDistancesOnGrid(
                              buildOpenPropGrid(router.gridId), effectiveTargets, router.minR, router.maxR)
                        : buildReversePathDistances(router.gridId, effectiveTargets, router.minR, router.maxR);
                }
                m_enemyPathDistByKi.emplace(router.ki, &it->second);
            }
        }
    }

    std::pair<bool, MilliFunds> UnifiedTeamEvaluator::computeOwnPropertyCaptureRisk(
        const ReplyPassEvaluation & eval, std::span<const int> vacateKills) const
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
            MilliFunds cost{0};
        };
        std::vector<std::pair<int, TilePoint>> friendlyOccupied;
        std::vector<FriendlyResponder> responders;
        std::vector<std::uint8_t> actorKiSeen(m_local.units.size(), 0);
        const auto addFriendlyResponder = [&](int ki, TilePoint pos, double hpF) {
            if (hpF <= 0.0 || pos == INVALID_TILE || std::ranges::find(vacateKills, ki) != vacateKills.end())
                return;
            friendlyOccupied.emplace_back(ki, pos);
            if (ki < 0 || ki >= static_cast<int>(m_local.units.size()) ||
                ki >= static_cast<int>(m_knowledge.units().size()))
                return;
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            const auto & knownF = m_knowledge.units()[static_cast<std::size_t>(ki)];
            const int minR = std::max(1, knownF.minRange);
            const int maxR = std::max(1, knownF.maxRange);
            const int moveB = minR > 1 ? 0 : std::max(0, fp.movement);
            responders.push_back({ki, pos, hpF, moveB, minR, maxR, fp.replacementCost});
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
            int capturePoints{0};
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
            if (rateE <= 0) continue;
            enemyCappers.push_back({e, ep.tile, hpE, stepsE, rateE, knownE.capturePoints, ep.movement, pawnTravelGrid(ep)});
        }
        if (enemyCappers.empty()) return {true, 0};

        const std::uint32_t occFriendlyGen = nextOccFriendlyGen();
        for (const auto & f : friendlyOccupied)
        {
            if (f.second.x >= 0 && f.second.x < m_local.width && f.second.y >= 0 && f.second.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(f.second.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(f.second.x);
                m_occFriendlyGen[cIdx] = occFriendlyGen;
                m_occFriendlyKi[cIdx] = f.first;
            }
        }
        const auto isFriendlyOccupied = [&](TilePoint t) {
            if (t.x < 0 || t.x >= m_local.width || t.y < 0 || t.y >= m_local.height)
                return false;
            return m_occFriendlyGen[static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) +
                                    static_cast<std::size_t>(t.x)] == occFriendlyGen;
        };
        const auto enemyCapperCanReach = [&](const EnemyCapper & ec, TilePoint goal) {
            return boundedCanReachGoal(ec.ki, ec.grid, ec.pos, goal, ec.movement,
                                       occFriendlyGen, INVALID_TILE, false);
        };

        std::stable_sort(responders.begin(), responders.end(), [](const FriendlyResponder & a, const FriendlyResponder & b) {
            const double sa = a.hp * static_cast<double>(std::max<MilliFunds>(1, a.cost));
            const double sb = b.hp * static_cast<double>(std::max<MilliFunds>(1, b.cost));
            return sa > sb;
        });

        std::vector<CaptureCoverActor> enemyDefenders;
        std::vector<std::uint8_t> enemyDefenderFiredE0;
        std::vector<std::pair<int, TilePoint>> survivingEnemies;
        for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
        {
            const auto & ep = m_local.units[static_cast<std::size_t>(e)];
            if (!ep.present || !ep.enemy)
                continue;
            double hpE = ep.realHp >= 0.0
                ? ep.realHp
                : (e < static_cast<int>(m_knowledge.units().size())
                       ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(e)].hpRounded)
                       : UNIFIED_HP_STEPS_DOUBLE);
            if (const auto it = eval.enemyReplyHp.find(e); it != eval.enemyReplyHp.end())
                hpE = static_cast<double>(it->second);
            else if (const auto itOwn = eval.enemyAfterOwn.find(e); itOwn != eval.enemyAfterOwn.end())
                hpE = static_cast<double>(itOwn->second);
            if (hpE <= 0.0)
                continue;
            const bool firedE0 = std::ranges::any_of(eval.shots, [&](const ReplyPassShot & s) {
                return s.shooterIndex == e;
            });
            const TilePoint ePos = (firedE0 && e < static_cast<int>(eval.combatLocations.size()) &&
                                    eval.combatLocations[static_cast<std::size_t>(e)] != INVALID_TILE)
                ? eval.combatLocations[static_cast<std::size_t>(e)]
                : ep.tile;
            survivingEnemies.emplace_back(e, ePos);
            const auto [eMinR, eMaxR] = unitAttackRange(e);
            const int eMoveB = eMinR > 1 ? 0 : std::max(0, ep.movement);
            if (ep.replacementCost > 0)
            {
                enemyDefenders.push_back({e, ePos, hpE, eMoveB, eMinR, eMaxR, ep.replacementCost});
                enemyDefenderFiredE0.push_back(firedE0 ? 1 : 0);
            }
        }

        std::vector<std::uint8_t> usedResponders(responders.size(), 0);
        std::vector<TilePoint> globalClaimedMeleeAdj;
        bool hqCaptureStartSafe = true;
        MilliFunds totalRisk = 0;
        for (const auto & prop : m_ownCapturableProperties)
        {
            if (isFriendlyOccupied(prop.tile)) continue;
            const EnemyCapper * strongestThreat = nullptr;
            bool immediateE0Finish = false;
            for (const auto & ec : enemyCappers)
            {
                if (ec.pos == prop.tile && ec.capturePoints > 0 && ec.capturePoints + ec.rate >= capPts)
                {
                    strongestThreat = &ec;
                    immediateE0Finish = true;
                    break;
                }
                const int startPts = ec.pos == prop.tile ? ec.capturePoints : 0;
                if (startPts + ec.rate * std::max<std::int32_t>(1, horizon - 1) < capPts)
                    continue;
                if (!enemyCapperCanReach(ec, prop.tile)) continue;
                if (!strongestThreat || ec.rate > strongestThreat->rate ||
                    (ec.rate == strongestThreat->rate && ec.hp > strongestThreat->hp))
                    strongestThreat = &ec;
            }
            if (!strongestThreat) continue;
            const std::uint32_t occEnemyGen = nextOccEnemyGen();
            for (const auto & se : survivingEnemies)
            {
                if (se.first == strongestThreat->ki || se.second == INVALID_TILE)
                    continue;
                if (se.second.x >= 0 && se.second.x < m_local.width &&
                    se.second.y >= 0 && se.second.y < m_local.height)
                {
                    const std::size_t cIdx =
                        static_cast<std::size_t>(se.second.y) * static_cast<std::size_t>(m_local.width) +
                        static_cast<std::size_t>(se.second.x);
                    m_occEnemyGen[cIdx] = occEnemyGen;
                    m_occEnemyKi[cIdx] = se.first;
                }
            }
            if (prop.tile.x >= 0 && prop.tile.x < m_local.width &&
                prop.tile.y >= 0 && prop.tile.y < m_local.height)
            {
                const std::size_t pIdx =
                    static_cast<std::size_t>(prop.tile.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(prop.tile.x);
                m_occEnemyGen[pIdx] = occEnemyGen;
                m_occEnemyKi[pIdx] = strongestThreat->ki;
            }
            const TerrainDefenseRule defRule = m_local.defense
                ? m_local.defense(strongestThreat->ki, prop.tile) : TerrainDefenseRule{};
            bool canInterrupt = false;
            if (!immediateE0Finish)
            {
                const int startPts = strongestThreat->pos == prop.tile ? strongestThreat->capturePoints : 0;
                const int followCaps = strongestThreat->rate * 2 >= capPts
                    ? 1 : std::max<std::int32_t>(1, horizon - 2);
                struct EligibleResponder
                {
                    std::size_t ri{0};
                    double hit{0.0};
                    std::uint8_t adjMask{0};
                    bool indirect{false};
                };
                std::vector<EligibleResponder> eligible;
                for (std::size_t ri = 0; ri < responders.size(); ++ri)
                {
                    if (usedResponders[ri] != 0)
                        continue;
                    const auto & fr = responders[ri];
                    std::uint8_t adjMask = 0;
                    if (fr.strikeRange > 1)
                    {
                        const int dist = manhattanDistance(fr.pos, prop.tile);
                        if (dist < fr.strikeRange || dist > fr.maxRange)
                            continue;
                        if (std::ranges::any_of(enemyDefenders, [&](const CaptureCoverActor & ed) {
                                return manhattanDistance(fr.pos, ed.pos) < fr.strikeRange &&
                                    canEnemyReachAttackOccupied(
                                        ed.ki, ed.pos, fr.pos, ed.moveBudget, ed.strikeRange, ed.maxRange,
                                        occFriendlyGen) &&
                                    maxWeaponBaseDamage(ed.ki, fr.ki, ed.strikeRange) > 0.0;
                            }))
                            continue;
                    }
                    else
                    {
                        for (std::uint8_t dirIdx = 0; dirIdx < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++dirIdx)
                        {
                            const StepVector step = entryStep(ENTRY_DIRECTIONS[dirIdx]);
                            const TilePoint adj{prop.tile.x + step.dx, prop.tile.y + step.dy};
                            if (adj.x < 0 || adj.x >= m_local.width || adj.y < 0 || adj.y >= m_local.height)
                                continue;
                            if (std::ranges::find(globalClaimedMeleeAdj, adj) != globalClaimedMeleeAdj.end())
                                continue;
                            if (isFriendlyOccupied(adj) && adj != fr.pos)
                                continue;
                            if (canFriendlyReachFiringSquare(fr.ki, fr.pos, adj, fr.moveBudget, occEnemyGen))
                                adjMask |= static_cast<std::uint8_t>(1u << dirIdx);
                        }
                        if (adjMask == 0)
                            continue;
                    }
                    const double baseDmg = maxWeaponBaseDamage(fr.ki, strongestThreat->ki, fr.strikeRange);
                    if (baseDmg <= 0.0) continue;
                    if (!prop.isHq)
                    {
                        const CaptureCoverActor frActor{
                            fr.ki, fr.pos, fr.hp, fr.moveBudget, fr.strikeRange, fr.maxRange, fr.cost};
                        const bool blockedByDefender = std::ranges::any_of(enemyDefenders, [&](const CaptureCoverActor & ed) {
                            if (ed.ki == strongestThreat->ki)
                                return false;
                            if (!canEnemyReachAttackOccupied(
                                    ed.ki, ed.pos, prop.tile, ed.moveBudget, ed.strikeRange, ed.maxRange,
                                    occFriendlyGen))
                                return false;
                            return !isCaptureTileCovered(frActor, ed, prop.tile, occEnemyGen, occFriendlyGen);
                        });
                        if (blockedByDefender)
                            continue;
                    }
                    const double approxHit = engineShotDamage(terrainReducedDamage(
                        defRule, baseDmg, fr.hp, ceilHpSteps(strongestThreat->hp)));
                    if (approxHit <= 0.0) continue;
                    eligible.push_back({ri, baseDmg, adjMask, fr.strikeRange > 1});
                }
                const auto attemptInterrupt = [&](std::uint8_t blockedMask, std::vector<std::size_t> & spentOut,
                                                  std::vector<TilePoint> & adjOut) {
                    spentOut.clear();
                    adjOut.clear();
                    double remHp = strongestThreat->hp;
                    std::uint8_t usedAdjMask = blockedMask;
                    for (const auto & item : eligible)
                    {
                        const auto & fr = responders[item.ri];
                        std::uint8_t chosenDirMask = 0;
                        TilePoint chosenAdj = INVALID_TILE;
                        if (!item.indirect)
                        {
                            const std::uint8_t freeMask = static_cast<std::uint8_t>(item.adjMask & ~usedAdjMask);
                            if (freeMask == 0)
                                continue;
                            for (std::uint8_t dirIdx = 0; dirIdx < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++dirIdx)
                            {
                                if ((freeMask & static_cast<std::uint8_t>(1u << dirIdx)) != 0)
                                {
                                    chosenDirMask = static_cast<std::uint8_t>(1u << dirIdx);
                                    const StepVector step = entryStep(ENTRY_DIRECTIONS[dirIdx]);
                                    chosenAdj = TilePoint{prop.tile.x + step.dx, prop.tile.y + step.dy};
                                    break;
                                }
                            }
                        }
                        const double hit = engineShotDamage(terrainReducedDamage(
                            defRule, item.hit, fr.hp, ceilHpSteps(remHp)));
                        if (hit <= 0.0) continue;
                        remHp = engineHpAfterReply(remHp, hit);
                        usedAdjMask |= chosenDirMask;
                        spentOut.push_back(item.ri);
                        if (chosenAdj != INVALID_TILE)
                            adjOut.push_back(chosenAdj);
                        const int remSteps = ceilHpSteps(remHp);
                        const int nextRate = replyCaptureRate(
                            strongestThreat->rate, std::max(0, strongestThreat->steps - remSteps), remSteps <= 0);
                        if (startPts + strongestThreat->rate + nextRate * followCaps < capPts)
                            return true;
                    }
                    return false;
                };
                std::vector<std::size_t> spentHere;
                std::vector<TilePoint> adjHere;
                if (!prop.isHq)
                {
                    canInterrupt = attemptInterrupt(0, spentHere, adjHere);
                }
                else
                {
                    std::vector<std::uint8_t> blockerDirMasks;
                    for (std::size_t ei = 0; ei < enemyDefenders.size(); ++ei)
                    {
                        if (enemyDefenderFiredE0[ei] != 0)
                            continue;
                        const auto & ed = enemyDefenders[ei];
                        if (ed.ki == strongestThreat->ki || ed.moveBudget <= 0)
                            continue;
                        std::uint8_t bMask = 0;
                        for (std::uint8_t dirIdx = 0; dirIdx < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++dirIdx)
                        {
                            const StepVector step = entryStep(ENTRY_DIRECTIONS[dirIdx]);
                            const TilePoint adj{prop.tile.x + step.dx, prop.tile.y + step.dy};
                            if (adj.x < 0 || adj.x >= m_local.width || adj.y < 0 || adj.y >= m_local.height)
                                continue;
                            const std::size_t aIdx =
                                static_cast<std::size_t>(adj.y) * static_cast<std::size_t>(m_local.width) +
                                static_cast<std::size_t>(adj.x);
                            if (m_occEnemyGen[aIdx] == occEnemyGen || m_occFriendlyGen[aIdx] == occFriendlyGen)
                                continue;
                            if (canEnemyReachFiringSquare(ed.ki, ed.pos, adj, ed.moveBudget, occFriendlyGen))
                                bMask |= static_cast<std::uint8_t>(1u << dirIdx);
                        }
                        if (bMask != 0)
                            blockerDirMasks.push_back(bMask);
                    }
                    constexpr int ADJ_SUBSET_LIMIT = 1 << 4;
                    bool adversariallyBlocked = false;
                    const std::uint8_t allRelevantAdj = std::accumulate(
                        eligible.begin(), eligible.end(), std::uint8_t{0},
                        [](std::uint8_t acc, const EligibleResponder & e) {
                            return static_cast<std::uint8_t>(acc | e.adjMask);
                        });
                    for (int sub = 0; sub < ADJ_SUBSET_LIMIT; ++sub)
                    {
                        const std::uint8_t subMask = static_cast<std::uint8_t>(sub);
                        if ((subMask & allRelevantAdj) != subMask)
                            continue;
                        if (subMask != 0)
                        {
                            std::vector<std::uint8_t> bits;
                            for (std::uint8_t d = 0; d < static_cast<std::uint8_t>(ENTRY_DIRECTIONS.size()); ++d)
                            {
                                if ((subMask & static_cast<std::uint8_t>(1u << d)) != 0)
                                    bits.push_back(d);
                            }
                            if (bits.size() > blockerDirMasks.size())
                                continue;
                            std::vector<int> matchBlocker(blockerDirMasks.size(), -1);
                            const auto canCover = [&](auto & self, std::size_t bitIdx,
                                                      std::vector<std::uint8_t> & seen) -> bool {
                                for (std::size_t bi = 0; bi < blockerDirMasks.size(); ++bi)
                                {
                                    if (seen[bi] != 0 || (blockerDirMasks[bi] & (1u << bits[bitIdx])) == 0)
                                        continue;
                                    seen[bi] = 1;
                                    if (matchBlocker[bi] < 0 || self(self, static_cast<std::size_t>(matchBlocker[bi]), seen))
                                    {
                                        matchBlocker[bi] = static_cast<int>(bitIdx);
                                        return true;
                                    }
                                }
                                return false;
                            };
                            bool matchedAll = true;
                            for (std::size_t bi = 0; bi < bits.size(); ++bi)
                            {
                                std::vector<std::uint8_t> seen(blockerDirMasks.size(), 0);
                                if (!canCover(canCover, bi, seen))
                                {
                                    matchedAll = false;
                                    break;
                                }
                            }
                            if (!matchedAll)
                                continue;
                        }
                        if (!attemptInterrupt(subMask, spentHere, adjHere))
                        {
                            adversariallyBlocked = true;
                            break;
                        }
                    }
                    if (!adversariallyBlocked)
                        canInterrupt = attemptInterrupt(0, spentHere, adjHere);
                }
                if (canInterrupt)
                {
                    for (const std::size_t sIdx : spentHere)
                        usedResponders[sIdx] = 1;
                    for (const TilePoint a : adjHere)
                        globalClaimedMeleeAdj.push_back(a);
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
                    // Stock already prices full enemyOptimum loss; charge only the 1-turn response cliff here.
                    const std::int32_t riskTurns = m_stockContext.valid() ? 1 : std::max(1, horizon - 1);
                    const MilliFunds swing = incomeSwing(
                        prop.income, riskTurns, OwnerSign::Ours, OwnerSign::Enemy);
                    MilliFunds loss = swing < 0 ? -swing : swing;
                    if (m_productionModel != nullptr && m_productionModel->valid())
                    {
                        const MilliFunds strat = m_productionModel->ownStrategicCaptureLoss(prop.tile, horizon);
                        const MilliFunds stratPart = m_stockContext.valid()
                            ? (strat / std::max<std::int32_t>(1, horizon - 1)) : strat;
                        RelativeStockDetail::checkedAdd(loss, stratPart);
                    }
                    RelativeStockDetail::checkedAdd(totalRisk, loss);
                }
            }
        }
        return {hqCaptureStartSafe, scalePaybackToRelevanceHorizon(totalRisk, horizon)};
    }

    int UnifiedTeamEvaluator::deployTurnsFromFactory(int ki, TilePoint tile) const
    {
        const int horizon = std::max(1, m_stockField.horizonTurns());
        if (m_friendlyFactories.empty() || ki < 0 || ki >= static_cast<int>(m_local.units.size()))
            return 0;
        const int move = std::max(1, m_local.units[static_cast<std::size_t>(ki)].movement);
        const int dist = cachedSetPathDistance(m_factoryPathDistByGrid, m_friendlyFactories, ki, tile);
        if (dist >= UNIFIED_UNREACHABLE_COST)
            return horizon;
        return std::min(horizon, (dist + move - 1) / move);
    }

    std::optional<MilliFunds> UnifiedTeamEvaluator::computeUnifiedRealizedMaterial(const ReplyPassEvaluation & eval) const
    {
        MilliFunds net = 0;
        for (const auto & actor : eval.actors)
        {
            if (actor.ownShot.target == NO_UNIT || isTerrainTargetId(actor.ownShot.target))
                continue;
            if (!RelativeStockDetail::checkedAdd(net, actor.ownShot.targetCapital) ||
                !RelativeStockDetail::checkedSubtract(net, actor.ownShot.counterCapital))
                return std::nullopt;
        }
        for (const auto & shot : eval.shots)
        {
            if (!RelativeStockDetail::checkedAdd(net, shot.counterCapital) ||
                !RelativeStockDetail::checkedSubtract(net, shot.targetCapital))
                return std::nullopt;
        }
        if (!m_local.ownFire)
        {
            for (const auto & actor : eval.actors)
            {
                if (actor.grantedDamage <= 0 || actor.ownShot.target != NO_UNIT)
                    continue;
                if (actor.actorSlot < 0 || actor.actorSlot >= static_cast<int>(m_problem.actors().size()))
                    continue;
                const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
                if (actor.candidateIndex < 0 || actor.candidateIndex >= static_cast<int>(member.candidates.size()))
                    continue;
                const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
                for (const auto & comp : cand.source.bundle.components)
                {
                    if (comp.kind == ComponentKind::Fire && !isTerrainTargetId(comp.fire.targetUnitId))
                    {
                        const MilliFunds cap =
                            (comp.fire.targetReplacementCost * actor.grantedDamage) / UNIT_HP_STEPS;
                        if (cap > 0 && !RelativeStockDetail::checkedAdd(net, cap))
                            return std::nullopt;
                        break;
                    }
                }
            }
        }
        for (const auto & actor : eval.actors)
        {
            if (actor.actorSlot < 0 || actor.actorSlot >= static_cast<int>(m_actorHasSafeStayCapture.size()) ||
                m_actorHasSafeStayCapture[static_cast<std::size_t>(actor.actorSlot)] == 0)
                continue;
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
            if (cand.capture.has_value() && cand.destination == cand.origin)
                continue;
            if (m_stockContext.valid() && m_stockField.columnSlotAt(cand.origin) != NO_STOCK_COLUMN)
                continue;
            const int ki = member.knowledgeUnitIndex;
            const int origPts = (ki >= 0 && ki < static_cast<int>(m_knowledge.units().size()))
                ? m_knowledge.units()[static_cast<std::size_t>(ki)].capturePoints : 0;
            const int capRate = (ki >= 0 && ki < static_cast<int>(m_knowledge.units().size()))
                ? m_knowledge.units()[static_cast<std::size_t>(ki)].captureRate : UNIT_HP_STEPS;
            const MilliFunds oneTurnSwing = propertyOneTurnSwingAt(cand.origin);
            const MilliFunds forfeited = (oneTurnSwing * origPts) / std::max(1, capRate);
            if (forfeited > 0 && !RelativeStockDetail::checkedSubtract(net, forfeited))
                return std::nullopt;
        }
        return net;
    }

    F1RetaliationResult UnifiedTeamEvaluator::computeF1RetaliationValue(
        const ReplyPassEvaluation & eval, std::span<const int> vacateKills) const
    {
        F1RetaliationResult res;
        if (eval.shots.empty() || (!m_local.weapon && !m_local.weaponSlots))
            return res;
        struct ReadyRetaliator
        {
            int ki{NO_UNIT};
            TilePoint pos{INVALID_TILE};
            double hp{0.0};
            int move{0};
            int minR{1};
            int maxR{1};
            MilliFunds cost{0};
            bool indirect{false};
            bool used{false};
        };
        std::vector<ReadyRetaliator> readyUnits;
        std::vector<std::uint8_t> friendlyActorSeen(m_local.units.size(), 0);
        const std::uint32_t occFriendlyGen = nextOccFriendlyGen();
        const auto stampFriendlyOcc = [&](int ki, TilePoint pos) {
            if (pos.x >= 0 && pos.x < m_local.width && pos.y >= 0 && pos.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(pos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(pos.x);
                m_occFriendlyGen[cIdx] = occFriendlyGen;
                m_occFriendlyKi[cIdx] = ki;
            }
        };
        for (const auto & actor : eval.actors)
        {
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const int ki = member.knowledgeUnitIndex;
            if (ki >= 0 && ki < static_cast<int>(friendlyActorSeen.size()))
                friendlyActorSeen[static_cast<std::size_t>(ki)] = 1;
            if (actor.stepsAfterReply <= 0 || ki < 0 || ki >= static_cast<int>(m_local.units.size()) ||
                std::ranges::find(vacateKills, ki) != vacateKills.end())
                continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
            stampFriendlyOcc(ki, cand.destination);
            if (member.stockRow != NO_STOCK_ROW)
                continue;
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            const auto [minR, maxR] = unitAttackRange(ki);
            double hp = static_cast<double>(actor.stepsAfterReply);
            if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                hp = it->second;
            readyUnits.push_back({ki, cand.destination, hp, std::max(0, fp.movement), minR, maxR,
                                  fp.replacementCost, minR > 1, false});
        }
        if (readyUnits.empty())
            return res;
        for (int ki = 0; ki < static_cast<int>(m_local.units.size()); ++ki)
        {
            if (friendlyActorSeen[static_cast<std::size_t>(ki)] != 0)
                continue;
            const auto & fp = m_local.units[static_cast<std::size_t>(ki)];
            if (!fp.present || fp.enemy || fp.tile == INVALID_TILE ||
                std::ranges::find(vacateKills, ki) != vacateKills.end())
                continue;
            double hp = fp.realHp >= 0.0 ? fp.realHp : UNIFIED_HP_STEPS_DOUBLE;
            if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                hp = it->second;
            if (hp > 0.0)
                stampFriendlyOcc(ki, fp.tile);
        }
        const std::uint32_t occEnemyGen = nextOccEnemyGen();
        for (int e = 0; e < static_cast<int>(m_local.units.size()); ++e)
        {
            const auto & ep = m_local.units[static_cast<std::size_t>(e)];
            if (!ep.present || !ep.enemy)
                continue;
            double hpE = ep.realHp >= 0.0
                ? ep.realHp
                : (e < static_cast<int>(m_knowledge.units().size())
                       ? static_cast<double>(m_knowledge.units()[static_cast<std::size_t>(e)].hpRounded)
                       : UNIFIED_HP_STEPS_DOUBLE);
            if (const auto it = eval.enemyReplyHp.find(e); it != eval.enemyReplyHp.end())
                hpE = static_cast<double>(it->second);
            else if (const auto itOwn = eval.enemyAfterOwn.find(e); itOwn != eval.enemyAfterOwn.end())
                hpE = static_cast<double>(itOwn->second);
            if (hpE <= 0.0)
                continue;
            TilePoint ePos = (e < static_cast<int>(eval.combatLocations.size()) &&
                              eval.combatLocations[static_cast<std::size_t>(e)] != INVALID_TILE)
                ? eval.combatLocations[static_cast<std::size_t>(e)]
                : ep.tile;
            for (const auto & s : eval.shots)
            {
                if (s.shooterIndex == e && s.firingTile != INVALID_TILE)
                {
                    ePos = s.firingTile;
                    break;
                }
            }
            if (ePos.x >= 0 && ePos.x < m_local.width && ePos.y >= 0 && ePos.y < m_local.height)
            {
                const std::size_t cIdx = static_cast<std::size_t>(ePos.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(ePos.x);
                m_occEnemyGen[cIdx] = occEnemyGen;
                m_occEnemyKi[cIdx] = e;
            }
        }

        const int horizon = std::max(1, m_stockField.horizonTurns());
        std::map<int, MilliFunds> e0DecisionStakeByShooter;
        std::map<int, MilliFunds> e0RealizedByShooter;
        for (const auto & s : eval.shots)
        {
            if (s.shooterIndex < 0)
                continue;
            const MilliFunds realizedStake = std::max<MilliFunds>(0, s.targetCapital - s.counterCapital);
            if (realizedStake > 0)
                RelativeStockDetail::checkedAdd(e0RealizedByShooter[s.shooterIndex], realizedStake);
            MilliFunds stake = realizedStake;
            bool isCapTarget = false;
            TilePoint capTile = INVALID_TILE;
            if (s.target < eval.actors.size())
            {
                const auto & act = eval.actors[s.target];
                if (act.actorSlot >= 0 && act.actorSlot < static_cast<int>(m_problem.actors().size()))
                {
                    const auto & mem = m_problem.actors()[static_cast<std::size_t>(act.actorSlot)];
                    if (act.candidateIndex >= 0 && act.candidateIndex < static_cast<int>(mem.candidates.size()))
                    {
                        const auto & c = mem.candidates[static_cast<std::size_t>(act.candidateIndex)];
                        if (c.capture.has_value())
                        {
                            isCapTarget = true;
                            capTile = c.destination;
                        }
                    }
                }
            }
            else
            {
                const std::size_t obsIdx = s.target - eval.actors.size();
                if (obsIdx < m_local.observedDefenders.size())
                {
                    const auto & obs = m_local.observedDefenders[obsIdx];
                    if (obs.knowledgeIndex >= 0 && obs.knowledgeIndex < static_cast<int>(m_knowledge.units().size()) &&
                        m_knowledge.units()[static_cast<std::size_t>(obs.knowledgeIndex)].capturePoints > 0)
                    {
                        isCapTarget = true;
                        capTile = obs.tile;
                    }
                }
            }
            if (isCapTarget && capTile != INVALID_TILE)
            {
                const int delayTurns = s.hpStepsAfter <= 0 ? std::max(1, horizon - 1) : 1;
                RelativeStockDetail::checkedAdd(stake, propertyOneTurnSwingAt(capTile) * delayTurns);
                if (m_productionModel != nullptr && m_productionModel->valid())
                {
                    RelativeStockDetail::checkedAdd(
                        stake, m_productionModel->enemyCaptureInterruptStrategicValue(capTile, delayTurns, horizon));
                }
            }
            if (stake > 0)
                RelativeStockDetail::checkedAdd(e0DecisionStakeByShooter[s.shooterIndex], stake);
        }

        std::vector<TilePoint> claimedF1Squares;
        std::map<int, double> shooterRemHp;
        for (const auto & shot : eval.shots)
        {
            const int e = shot.shooterIndex;
            if (e < 0 || e >= static_cast<int>(m_local.units.size()) || shot.firingTile == INVALID_TILE)
                continue;
            const auto & ep = m_local.units[static_cast<std::size_t>(e)];
            if (!ep.present || !ep.enemy || ep.replacementCost <= 0)
                continue;
            auto [hpIt, inserted] = shooterRemHp.try_emplace(e, ep.realHp >= 0.0 ? ep.realHp : static_cast<double>(UNIT_HP_STEPS));
            if (inserted)
            {
                if (const auto it = eval.enemyReplyHp.find(e); it != eval.enemyReplyHp.end())
                    hpIt->second = static_cast<double>(it->second);
                else if (const auto itOwn = eval.enemyAfterOwn.find(e); itOwn != eval.enemyAfterOwn.end())
                    hpIt->second = static_cast<double>(itOwn->second);
            }
            if (hpIt->second <= 0.0)
                continue;

            const TerrainDefenseRule eDef = m_local.defense
                ? m_local.defense(e, shot.firingTile) : TerrainDefenseRule{};
            int bestIdx = -1;
            MilliFunds bestNet = 0;
            double bestAfterE = hpIt->second;
            TilePoint bestMeleeSq = INVALID_TILE;
            for (std::size_t ri = 0; ri < readyUnits.size(); ++ri)
            {
                const auto & r = readyUnits[ri];
                if (r.used)
                    continue;
                const double baseDmg = maxWeaponBaseDamage(r.ki, e, r.minR);
                if (baseDmg <= 0.0)
                    continue;
                if (r.indirect)
                {
                    const int dist = manhattanDistance(r.pos, shot.firingTile);
                    if (dist < r.minR || dist > r.maxR)
                        continue;
                    const double rawHit = terrainReducedDamage(eDef, baseDmg, r.hp, ceilHpSteps(hpIt->second));
                    const double hit = engineShotDamage(rawHit);
                    if (hit <= 0.0)
                        continue;
                    const double eAfter = engineHpAfterReply(hpIt->second, hit);
                    const MilliFunds eLoss = combatCapitalLoss(ep.replacementCost, hpIt->second, eAfter);
                    if (eLoss > bestNet)
                    {
                        bestNet = eLoss;
                        bestIdx = static_cast<int>(ri);
                        bestAfterE = eAfter;
                        bestMeleeSq = INVALID_TILE;
                    }
                    continue;
                }
                const double rawHit = terrainReducedDamage(eDef, baseDmg, r.hp, ceilHpSteps(hpIt->second));
                const double hit = engineShotDamage(rawHit);
                if (hit <= 0.0)
                    continue;
                const double eAfter = engineHpAfterReply(hpIt->second, hit);
                const MilliFunds eLoss = combatCapitalLoss(ep.replacementCost, hpIt->second, eAfter);
                const double cBase = eAfter > 0.0 ? maxWeaponBaseDamage(e, r.ki, 1) : 0.0;
                for (const EntryDirection dir : ENTRY_DIRECTIONS)
                {
                    const StepVector step = entryStep(dir);
                    const TilePoint adj{shot.firingTile.x + step.dx, shot.firingTile.y + step.dy};
                    if (adj.x < 0 || adj.x >= m_local.width || adj.y < 0 || adj.y >= m_local.height)
                        continue;
                    if (std::ranges::find(claimedF1Squares, adj) != claimedF1Squares.end())
                        continue;
                    const std::size_t aIdx =
                        static_cast<std::size_t>(adj.y) * static_cast<std::size_t>(m_local.width) +
                        static_cast<std::size_t>(adj.x);
                    if (m_occFriendlyGen[aIdx] == occFriendlyGen && adj != r.pos)
                        continue;
                    if (!canFriendlyReachFiringSquare(r.ki, r.pos, adj, r.move, occEnemyGen))
                        continue;
                    MilliFunds rLoss = 0;
                    if (cBase > 0.0)
                    {
                        const TerrainDefenseRule rDef = m_local.defense
                            ? m_local.defense(r.ki, adj) : TerrainDefenseRule{};
                        const double rawC = sameActionCounterDamage(
                            rDef, cBase, hpIt->second, rawHit, ceilHpSteps(r.hp));
                        rLoss = combatCapitalLoss(r.cost, r.hp, engineHpAfterReply(r.hp, engineShotDamage(rawC)));
                    }
                    const MilliFunds net = eLoss - rLoss;
                    if (net > bestNet)
                    {
                        bestNet = net;
                        bestIdx = static_cast<int>(ri);
                        bestAfterE = eAfter;
                        bestMeleeSq = adj;
                    }
                }
            }
            if (bestIdx >= 0 && bestNet > 0)
            {
                // Record both executed F1 continuation and deterred-shooter abstention for minimax comparison.
                const int retKi = readyUnits[static_cast<std::size_t>(bestIdx)].ki;
                const TilePoint retDest = bestMeleeSq != INVALID_TILE
                    ? bestMeleeSq : readyUnits[static_cast<std::size_t>(bestIdx)].pos;
                readyUnits[static_cast<std::size_t>(bestIdx)].used = true;
                if (bestMeleeSq != INVALID_TILE)
                    claimedF1Squares.push_back(bestMeleeSq);
                RelativeStockDetail::checkedAdd(res.total, bestNet);
                res.executedEnemyAfterF1[e] = bestAfterE;
                res.executedUsedFriendlies.push_back(retKi);
                res.executedFriendlyNextPos[retKi] = retDest;

                MilliFunds & remStake = e0DecisionStakeByShooter[e];
                MilliFunds & remRealized = e0RealizedByShooter[e];
                if (bestNet <= remStake)
                {
                    remStake -= bestNet;
                    remRealized = std::max<MilliFunds>(0, remRealized - bestNet);
                    hpIt->second = bestAfterE;
                    res.enemyAfterF1[e] = bestAfterE;
                    res.usedFriendlies.push_back(retKi);
                    res.friendlyNextPos[retKi] = retDest;
                    RelativeStockDetail::checkedAdd(res.minimaxTotal, bestNet);
                }
                else
                {
                    if (std::ranges::find(res.deterredShooters, e) == res.deterredShooters.end())
                        res.deterredShooters.push_back(e);
                    if (remRealized > 0)
                        RelativeStockDetail::checkedAdd(res.minimaxTotal, remRealized);
                    remRealized = 0;
                    remStake = 0;
                }
            }
        }
        return res;
    }

    MilliFunds UnifiedTeamEvaluator::computeRepairValue(const ReplyPassEvaluation & eval,
                                  std::span<const int> vacateKills,
                                  std::span<const int> deterredShooters) const
    {
        const int horizon = std::max(1, m_stockField.horizonTurns());
        const bool zeroFactoryMap = m_friendlyFactories.empty();
        MilliFunds total = 0;
        for (const auto & actor : eval.actors)
        {
            const auto & member = m_problem.actors()[static_cast<std::size_t>(actor.actorSlot)];
            const int ki = member.knowledgeUnitIndex;
            if (ki < 0 || ki >= static_cast<int>(m_local.units.size()) ||
                ki >= static_cast<int>(m_knowledge.units().size()))
                continue;
            const auto & pawn = m_local.units[static_cast<std::size_t>(ki)];
            const MilliFunds cost = pawn.replacementCost;
            if (cost <= 0)
                continue;
            const auto & cand = member.candidates[static_cast<std::size_t>(actor.candidateIndex)];
            const int deployTurns = zeroFactoryMap ? 0 : deployTurnsFromFactory(ki, cand.source.bundle.origin);
            const MilliFunds embodiedCap = (cost * deployTurns) / horizon;
            double preDeterredHp = -1.0;
            bool hitOnlyByDeterred = false;
            if (!deterredShooters.empty())
            {
                bool anyHit = false;
                bool allDet = true;
                for (const ReplyPassShot & s : eval.shots)
                {
                    if (s.targetKnowledge != ki)
                        continue;
                    if (std::ranges::find(deterredShooters, s.shooterIndex) == deterredShooters.end())
                    {
                        allDet = false;
                        break;
                    }
                    if (!anyHit)
                        preDeterredHp = s.realBefore;
                    anyHit = true;
                }
                hitOnlyByDeterred = anyHit && allDet && preDeterredHp > 0.0;
            }
            if ((!hitOnlyByDeterred && actor.stepsAfterReply <= 0) ||
                std::ranges::find(vacateKills, ki) != vacateKills.end())
            {
                if (embodiedCap > 0)
                    RelativeStockDetail::checkedSubtract(total, embodiedCap);
                const int hpStart = m_knowledge.units()[static_cast<std::size_t>(ki)].hpRounded;
                if (zeroFactoryMap && !m_friendlyRepairSites.empty() && hpStart < UNIT_HP_STEPS)
                {
                    const int lostRepairSteps = std::min(UNIFIED_CITY_REPAIR_STEPS, UNIT_HP_STEPS - hpStart);
                    RelativeStockDetail::checkedSubtract(total, (cost * lostRepairSteps) / UNIT_HP_STEPS);
                }
                continue;
            }
            if (m_friendlyRepairSites.empty())
                continue;
            double hpAfter = hitOnlyByDeterred ? preDeterredHp : static_cast<double>(actor.stepsAfterReply);
            if (!hitOnlyByDeterred)
            {
                if (const auto it = eval.realAfterReply.find(ki); it != eval.realAfterReply.end())
                    hpAfter = it->second;
            }
            if (hpAfter <= 0.0 || hpAfter >= UNIFIED_HP_STEPS_DOUBLE)
                continue;
            const double healSteps = std::min(
                static_cast<double>(UNIFIED_CITY_REPAIR_STEPS), UNIFIED_HP_STEPS_DOUBLE - hpAfter);
            int minRepairDist = UNIFIED_UNREACHABLE_COST;
            bool canHealAtDest = false;
            if (ki >= 0 && ki < static_cast<int>(m_repairPathDistByKi.size()) &&
                cand.destination.x >= 0 && cand.destination.x < m_local.width &&
                cand.destination.y >= 0 && cand.destination.y < m_local.height)
            {
                const auto * distVec = m_repairPathDistByKi[static_cast<std::size_t>(ki)];
                const auto * maskVec = m_repairSiteMaskByKi[static_cast<std::size_t>(ki)];
                if (distVec == nullptr || maskVec == nullptr)
                    continue;
                const std::size_t cIdx =
                    static_cast<std::size_t>(cand.destination.y) * static_cast<std::size_t>(m_local.width) +
                    static_cast<std::size_t>(cand.destination.x);
                if (cIdx < maskVec->size() && (*maskVec)[cIdx] != 0)
                {
                    minRepairDist = 0;
                    canHealAtDest = true;
                }
                else if (cIdx < distVec->size() && (*distVec)[cIdx] < std::numeric_limits<std::int32_t>::max())
                {
                    minRepairDist = (*distVec)[cIdx];
                }
            }
            else
            {
                for (const TilePoint site : m_friendlyRepairSites)
                {
                    if (m_local.canRepairOnTerrain && !m_local.canRepairOnTerrain(ki, site))
                        continue;
                    if (site == cand.destination)
                    {
                        minRepairDist = 0;
                        canHealAtDest = true;
                        break;
                    }
                    minRepairDist = std::min(minRepairDist, tilePathDistance(ki, cand.destination, site, false));
                }
            }
            if (minRepairDist >= UNIFIED_UNREACHABLE_COST)
                continue;
            const int move = std::max(1, pawn.movement);
            const int turnsToRepair = canHealAtDest ? 0 : 1 + (minRepairDist + move - 1) / move;
            if (turnsToRepair >= horizon)
                continue;
            const MilliFunds baseCap = zeroFactoryMap ? cost : embodiedCap;
            const MilliFunds gross = static_cast<MilliFunds>(std::llround(
                (static_cast<double>(baseCap) * healSteps) / UNIFIED_HP_STEPS_DOUBLE));
            const MilliFunds disc = (gross * (horizon - turnsToRepair)) / horizon;
            const MilliFunds liveCap = static_cast<MilliFunds>(std::llround(
                (static_cast<double>(baseCap) * hpAfter) / UNIFIED_HP_STEPS_DOUBLE));
            const MilliFunds offlineCost = (liveCap * 2 * turnsToRepair) / horizon;
            const MilliFunds netRepair = disc - offlineCost;
            if (netRepair > 0)
                RelativeStockDetail::checkedAdd(total, netRepair);
        }
        return total;
    }

    std::uint32_t UnifiedTeamEvaluator::nextBfsGen() const
    {
        if (++m_curBfsGen == 0)
        {
            std::fill(m_bfsGen.begin(), m_bfsGen.end(), 0);
            m_curBfsGen = 1;
        }
        return m_curBfsGen;
    }

    std::uint32_t UnifiedTeamEvaluator::nextOccFriendlyGen() const
    {
        if (m_fast)
            m_fast->friendlyReach.clear();
        if (++m_curOccFriendlyGen == 0)
        {
            std::fill(m_occFriendlyGen.begin(), m_occFriendlyGen.end(), 0);
            m_curOccFriendlyGen = 1;
        }
        return m_curOccFriendlyGen;
    }

    std::uint32_t UnifiedTeamEvaluator::nextOccEnemyGen() const
    {
        if (m_fast)
            m_fast->enemyReach.clear();
        if (++m_curOccEnemyGen == 0)
        {
            std::fill(m_occEnemyGen.begin(), m_occEnemyGen.end(), 0);
            m_curOccEnemyGen = 1;
        }
        return m_curOccEnemyGen;
    }

    bool UnifiedTeamEvaluator::hasAdjacentZoc(TilePoint cur, int moverKi, std::uint32_t occGen, TilePoint ignoredTile,
                        const std::vector<std::uint32_t> & genGrid,
                        const std::vector<int> & kiGrid) const
    {
        for (const EntryDirection dir : ENTRY_DIRECTIONS)
        {
            const StepVector step = entryStep(dir);
            const TilePoint nb{cur.x + step.dx, cur.y + step.dy};
            if (nb.x < 0 || nb.x >= m_local.width || nb.y < 0 || nb.y >= m_local.height || nb == ignoredTile)
                continue;
            const std::size_t nIdx = static_cast<std::size_t>(nb.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(nb.x);
            if (genGrid[nIdx] == occGen && kiGrid[nIdx] >= 0 && canExertZocOn(kiGrid[nIdx], moverKi))
                return true;
        }
        return false;
    }

    bool UnifiedTeamEvaluator::boundedCanReachGoalOnOcc(int moverKi, int gridId, TilePoint start, TilePoint goal, int moveBudget,
                                  std::uint32_t occGen, TilePoint ignoredTile,
                                  const std::vector<std::uint32_t> & genGrid,
                                  const std::vector<int> & kiGrid) const
    {
        if (start == goal)
            return true;
        const int effGrid = (gridId >= 0 && gridId < static_cast<int>(m_local.grids.size()))
            ? gridId : (!m_local.grids.empty() ? 0 : -1);
        if (moveBudget <= 0 || effGrid < 0 || m_cellCount == 0)
            return false;
        const auto & grid = m_local.grids[static_cast<std::size_t>(effGrid)];
        if (!grid.onGrid(start.x, start.y) || !grid.onGrid(goal.x, goal.y))
            return false;
        const bool ignoresCollision = (moverKi >= 0 && moverKi < static_cast<int>(m_local.units.size()))
            ? m_local.units[static_cast<std::size_t>(moverKi)].ignoresCollision : false;
        const auto idxOf = [&](TilePoint t) {
            return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(t.x);
        };
        const std::size_t startIdx = idxOf(start);
        const std::size_t goalIdx = idxOf(goal);

        OccReachCache * cache = nullptr;
        if (m_fast && m_fast->bitWords > 0 && occGen != 0)
        {
            if (&genGrid == &m_occFriendlyGen && occGen == m_curOccFriendlyGen)
                cache = &m_fast->friendlyReach;
            else if (&genGrid == &m_occEnemyGen && occGen == m_curOccEnemyGen)
                cache = &m_fast->enemyReach;
        }
        if (cache != nullptr)
        {
            const std::size_t words = m_fast->bitWords;
            const auto testBit = [&](std::size_t off, std::size_t idx) -> bool {
                return ((cache->bitPool[off + (idx >> TEAM_BIT_WORD_SHIFT)] >> (idx & TEAM_BIT_WORD_MASK)) & 1ULL) != 0;
            };
            const auto runFlood = [&](TilePoint ign, bool trackTouched) -> ReachBitEntry {
                const std::size_t reachOff = cache->bitPool.size();
                cache->bitPool.resize(reachOff + words * (trackTouched ? 2 : 1), 0ULL);
                const std::size_t touchedOff = trackTouched ? (reachOff + words) : 0;
                const std::uint32_t gen = nextBfsGen();
                using Node = std::pair<std::int32_t, TilePoint>;
                const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
                m_bfsHeap.clear();
                m_bfsGen[startIdx] = gen;
                m_bfsDist[startIdx] = 0;
                m_bfsHeap.push_back({0, start});
                std::push_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
                while (!m_bfsHeap.empty())
                {
                    std::pop_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
                    const auto [d, cur] = m_bfsHeap.back();
                    m_bfsHeap.pop_back();
                    const std::size_t curIdx = idxOf(cur);
                    if (d > m_bfsDist[curIdx])
                        continue;
                    cache->bitPool[reachOff + (curIdx >> TEAM_BIT_WORD_SHIFT)] |=
                        (1ULL << (curIdx & TEAM_BIT_WORD_MASK));
                    if (!ignoresCollision && cur != start)
                    {
                        bool zocStopped = false;
                        for (const EntryDirection dir : ENTRY_DIRECTIONS)
                        {
                            const StepVector step = entryStep(dir);
                            const TilePoint nb{cur.x + step.dx, cur.y + step.dy};
                            if (nb.x < 0 || nb.x >= m_local.width || nb.y < 0 || nb.y >= m_local.height || nb == ign)
                                continue;
                            const std::size_t nIdx = idxOf(nb);
                            if (genGrid[nIdx] == occGen && kiGrid[nIdx] >= 0 && canExertZocOn(kiGrid[nIdx], moverKi))
                            {
                                zocStopped = true;
                                if (trackTouched)
                                    cache->bitPool[touchedOff + (nIdx >> TEAM_BIT_WORD_SHIFT)] |=
                                        (1ULL << (nIdx & TEAM_BIT_WORD_MASK));
                                else
                                    break;
                            }
                        }
                        if (zocStopped)
                            continue;
                    }
                    for (const EntryDirection dir : ENTRY_DIRECTIONS)
                    {
                        const StepVector step = entryStep(dir);
                        const TilePoint next{cur.x + step.dx, cur.y + step.dy};
                        if (!grid.onGrid(next.x, next.y))
                            continue;
                        const std::size_t nextIdx = idxOf(next);
                        if (!ignoresCollision && genGrid[nextIdx] == occGen && next != ign)
                        {
                            if (trackTouched)
                                cache->bitPool[touchedOff + (nextIdx >> TEAM_BIT_WORD_SHIFT)] |=
                                    (1ULL << (nextIdx & TEAM_BIT_WORD_MASK));
                            continue;
                        }
                        const std::int32_t c = grid.cost(next.x, next.y, dir);
                        if (c == IMPASSABLE_COST)
                            continue;
                        const std::int32_t nd = d + c;
                        if (nd <= moveBudget && (m_bfsGen[nextIdx] != gen || nd < m_bfsDist[nextIdx]))
                        {
                            m_bfsGen[nextIdx] = gen;
                            m_bfsDist[nextIdx] = nd;
                            m_bfsHeap.push_back({nd, next});
                            std::push_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
                        }
                    }
                }
                return {reachOff, touchedOff};
            };

            const ReachCacheKey baseKey{moverKi, effGrid, static_cast<int>(startIdx), moveBudget, -1};
            auto baseIt = cache->entries.find(baseKey);
            if (baseIt == cache->entries.end())
                baseIt = cache->entries.emplace(baseKey, runFlood(INVALID_TILE, true)).first;
            const ReachBitEntry baseEntry = baseIt->second;

            const int ignoredIdx = (ignoredTile != INVALID_TILE && grid.onGrid(ignoredTile.x, ignoredTile.y))
                ? static_cast<int>(idxOf(ignoredTile))
                : -1;
            if (ignoredIdx < 0 || ignoresCollision ||
                !testBit(baseEntry.touchedOff, static_cast<std::size_t>(ignoredIdx)))
                return testBit(baseEntry.reachOff, goalIdx);

            const ReachCacheKey ignKey{moverKi, effGrid, static_cast<int>(startIdx), moveBudget, ignoredIdx};
            auto ignIt = cache->entries.find(ignKey);
            if (ignIt == cache->entries.end())
                ignIt = cache->entries.emplace(ignKey, runFlood(ignoredTile, false)).first;
            return testBit(ignIt->second.reachOff, goalIdx);
        }

        const std::uint32_t gen = nextBfsGen();
        using Node = std::pair<std::int32_t, TilePoint>;
        const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
        m_bfsHeap.clear();
        m_bfsGen[startIdx] = gen;
        m_bfsDist[startIdx] = 0;
        m_bfsHeap.push_back({0, start});
        std::push_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
        while (!m_bfsHeap.empty())
        {
            std::pop_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
            const auto [d, cur] = m_bfsHeap.back();
            m_bfsHeap.pop_back();
            if (cur == goal)
                return true;
            if (d > m_bfsDist[idxOf(cur)])
                continue;
            if (!ignoresCollision && cur != start &&
                hasAdjacentZoc(cur, moverKi, occGen, ignoredTile, genGrid, kiGrid))
                continue;
            for (const EntryDirection dir : ENTRY_DIRECTIONS)
            {
                const StepVector step = entryStep(dir);
                const TilePoint next{cur.x + step.dx, cur.y + step.dy};
                if (!grid.onGrid(next.x, next.y))
                    continue;
                const std::size_t nextIdx = idxOf(next);
                if (!ignoresCollision && genGrid[nextIdx] == occGen && next != ignoredTile)
                    continue;
                const std::int32_t c = grid.cost(next.x, next.y, dir);
                if (c == IMPASSABLE_COST)
                    continue;
                const std::int32_t nd = d + c;
                if (nd <= moveBudget && (m_bfsGen[nextIdx] != gen || nd < m_bfsDist[nextIdx]))
                {
                    m_bfsGen[nextIdx] = gen;
                    m_bfsDist[nextIdx] = nd;
                    m_bfsHeap.push_back({nd, next});
                    std::push_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
                }
            }
        }
        return false;
    }

    bool UnifiedTeamEvaluator::boundedCanReachGoal(int moverKi, int gridId, TilePoint start, TilePoint goal, int moveBudget,
                             std::uint32_t occFriendlyGen, TilePoint ignoredFriendly, bool allowCollision) const
    {
        (void)allowCollision;
        return boundedCanReachGoalOnOcc(moverKi, gridId, start, goal, moveBudget, occFriendlyGen, ignoredFriendly,
                                        m_occFriendlyGen, m_occFriendlyKi);
    }

    void UnifiedTeamEvaluator::collectReachableEscapeTiles(int victimKi, int gridId, TilePoint start, int moveBudget,
                                     bool ignoresCollision, std::uint32_t occEnemyGen) const
    {
        m_escapeTilesScratch.clear();
        if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()) || moveBudget <= 0 || m_cellCount == 0)
            return;
        const auto & grid = m_local.grids[static_cast<std::size_t>(gridId)];
        if (!grid.onGrid(start.x, start.y))
            return;
        const std::uint32_t gen = nextBfsGen();
        const auto idxOf = [&](TilePoint t) {
            return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(t.x);
        };
        using Node = std::pair<std::int32_t, TilePoint>;
        const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
        m_bfsHeap.clear();
        const std::size_t startIdx = idxOf(start);
        m_bfsGen[startIdx] = gen;
        m_bfsDist[startIdx] = 0;
        m_bfsHeap.push_back({0, start});
        std::push_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
        while (!m_bfsHeap.empty())
        {
            std::pop_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
            const auto [d, cur] = m_bfsHeap.back();
            m_bfsHeap.pop_back();
            if (d > m_bfsDist[idxOf(cur)])
                continue;
            m_escapeTilesScratch.push_back(cur);
            if (cur != start &&
                hasAdjacentZoc(cur, victimKi, occEnemyGen, INVALID_TILE, m_occEnemyGen, m_occEnemyKi))
                continue;
            for (const EntryDirection dir : ENTRY_DIRECTIONS)
            {
                const StepVector step = entryStep(dir);
                const TilePoint next{cur.x + step.dx, cur.y + step.dy};
                if (!grid.onGrid(next.x, next.y))
                    continue;
                const std::size_t nextIdx = idxOf(next);
                if (!ignoresCollision && m_occEnemyGen[nextIdx] == occEnemyGen)
                    continue;
                const std::int32_t c = grid.cost(next.x, next.y, dir);
                if (c == IMPASSABLE_COST)
                    continue;
                const std::int32_t nd = d + c;
                if (nd <= moveBudget && (m_bfsGen[nextIdx] != gen || nd < m_bfsDist[nextIdx]))
                {
                    m_bfsGen[nextIdx] = gen;
                    m_bfsDist[nextIdx] = nd;
                    m_bfsHeap.push_back({nd, next});
                    std::push_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
                }
            }
        }
    }

    void UnifiedTeamEvaluator::collectEnemyEscapeTiles(int enemyKi, int gridId, TilePoint start, int moveBudget,
                                 bool ignoresCollision, std::uint32_t occFriendlyGen,
                                 std::uint32_t occEnemyGen) const
    {
        m_escapeTilesScratch.clear();
        if (gridId < 0 || gridId >= static_cast<int>(m_local.grids.size()) || moveBudget <= 0 || m_cellCount == 0)
            return;
        const auto & grid = m_local.grids[static_cast<std::size_t>(gridId)];
        if (!grid.onGrid(start.x, start.y))
            return;
        const std::uint32_t gen = nextBfsGen();
        const auto idxOf = [&](TilePoint t) {
            return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_local.width) +
                static_cast<std::size_t>(t.x);
        };
        using Node = std::pair<std::int32_t, TilePoint>;
        const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
        m_bfsHeap.clear();
        const std::size_t startIdx = idxOf(start);
        m_bfsGen[startIdx] = gen;
        m_bfsDist[startIdx] = 0;
        m_bfsHeap.push_back({0, start});
        std::push_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
        while (!m_bfsHeap.empty())
        {
            std::pop_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
            const auto [d, cur] = m_bfsHeap.back();
            m_bfsHeap.pop_back();
            const std::size_t curIdx = idxOf(cur);
            if (d > m_bfsDist[curIdx])
                continue;
            if (cur == start || m_occEnemyGen[curIdx] != occEnemyGen)
                m_escapeTilesScratch.push_back(cur);
            if (!ignoresCollision && cur != start &&
                hasAdjacentZoc(cur, enemyKi, occFriendlyGen, INVALID_TILE, m_occFriendlyGen, m_occFriendlyKi))
                continue;
            for (const EntryDirection dir : ENTRY_DIRECTIONS)
            {
                const StepVector step = entryStep(dir);
                const TilePoint next{cur.x + step.dx, cur.y + step.dy};
                if (!grid.onGrid(next.x, next.y))
                    continue;
                const std::size_t nextIdx = idxOf(next);
                if (!ignoresCollision && m_occFriendlyGen[nextIdx] == occFriendlyGen)
                    continue;
                const std::int32_t c = grid.cost(next.x, next.y, dir);
                if (c == IMPASSABLE_COST)
                    continue;
                const std::int32_t nd = d + c;
                if (nd <= moveBudget && (m_bfsGen[nextIdx] != gen || nd < m_bfsDist[nextIdx]))
                {
                    m_bfsGen[nextIdx] = gen;
                    m_bfsDist[nextIdx] = nd;
                    m_bfsHeap.push_back({nd, next});
                    std::push_heap(m_bfsHeap.begin(), m_bfsHeap.end(), cmp);
                }
            }
        }
    }

    UnifiedTeamEvaluator::~UnifiedTeamEvaluator()
    {
        if (!m_profileEnabled || m_evals <= 0)
            return;
        if (std::FILE * fp = std::fopen(m_profilePath, "a"))
        {
            const auto ms = [](std::int64_t ns) { return static_cast<double>(ns) / 1e6; };
            std::fprintf(fp,
                "evals=%d scored=%lld reused=%lld pruned=%d reply=%.1f hq=%.1f real=%.1f vac=%.1f f1=%.1f stock=%.1f fwd=%.1f den=%.1f own=%.1f trap=%.1f fac=%.1f rep=%.1f\n",
                m_evals, static_cast<long long>(m_profScored), static_cast<long long>(m_profReused), m_pruned,
                ms(m_profReplyNs), ms(m_profHqNs), ms(m_profRealizedNs), ms(m_profVacateNs),
                ms(m_profF1Ns), ms(m_profStockNs), ms(m_profFwdCovNs), ms(m_profDenialNs),
                ms(m_profOwnRiskNs), ms(m_profTrapNs), ms(m_profFactoryNs), ms(m_profRepairBrkNs));
            std::fclose(fp);
        }
    }

} // namespace Coordinator
