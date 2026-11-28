#pragma once

#include "ai/coordinator/opentacticalrefinement.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/replystockcontext.h"
#include "ai/coordinator/unifiedproduction.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Coordinator
{
    // Exact game rules and search work budgets.
    inline constexpr MilliFunds UNIFIED_NEUTRAL_PROPERTY_INCOME = 1'000'000;
    inline constexpr std::int64_t UNIFIED_SEARCH_WORK_BUDGET = 240'000'000;
    inline constexpr std::int64_t UNIFIED_SEARCH_WORK_PER_ACTOR = 40'000'000;
    inline constexpr std::int64_t UNIFIED_FINAL_REPLAY_WORK_FLOOR = 2'000'000;
    inline constexpr std::int32_t UNIFIED_STOCK_TUNE_ROUNDS = 40;
    inline constexpr std::int32_t UNIFIED_CITY_REPAIR_STEPS = 2;
    inline constexpr std::int32_t UNIFIED_UNREACHABLE_COST = std::numeric_limits<std::int32_t>::max() / 4;
    inline constexpr std::size_t UNIFIED_OPEN_SITE_MASK_BITS = 64;
    inline constexpr double UNIFIED_HP_STEPS_DOUBLE = 10.0;

    struct UnifiedTeamScore
    {
        bool valid{false};
        std::tuple<TerminalClass, int, int, MilliFunds> worstHqRank{
            TerminalClass::ForcedLoss, std::numeric_limits<int>::min(),
            std::numeric_limits<int>::min(), std::numeric_limits<MilliFunds>::min()};
        MilliFunds worstEconomicValue{std::numeric_limits<MilliFunds>::min()};
        // Negative path cost sum breaks exact-MilliFund ties beyond ETA >= H.
        std::int64_t geodesicProgress{std::numeric_limits<std::int64_t>::min()};
        MilliFunds realizedMaterial{0};
        MilliFunds postReplyStock{0};
        MilliFunds forwardCoverage{0};
        MilliFunds enemyDenial{0};
        MilliFunds ownCaptureRisk{0};
        MilliFunds factoryValue{0};
        int affordableBuildSlots{0};
        TacticalReply bindingPolicy{TacticalReply::Economic};
        int bindingTargetUnit{NO_UNIT};

        auto rankingKey() const
        {
            return std::tuple{std::get<0>(worstHqRank), worstEconomicValue, geodesicProgress};
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

    std::vector<PlanRowAction> buildPostReplyStockActions(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        const ReplyPassEvaluation & eval);


    int mostValuableExposedUnit(
        const MasterAssignmentProblem & problem, const CombatTerrain & local, std::span<const int> choices);


    std::vector<int> allExposedUnits(
        const MasterAssignmentProblem & problem, const CombatTerrain & local, std::span<const int> choices);


    std::vector<int> allExposedActiveCappers(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        const CombatTerrain & local, std::span<const int> choices);


    struct F1RetaliationResult
    {
        MilliFunds total{0};
        MilliFunds minimaxTotal{0};
        std::map<int, double> enemyAfterF1;
        std::vector<int> usedFriendlies;
        std::map<int, TilePoint> friendlyNextPos;
        std::vector<int> deterredShooters;
        std::map<int, double> executedEnemyAfterF1;
        std::vector<int> executedUsedFriendlies;
        std::map<int, TilePoint> executedFriendlyNextPos;
    };

    using UnifiedHqRankFn = std::function<std::optional<std::tuple<TerminalClass, int, int, MilliFunds>>(const ReplyPassEvaluation &)>;
    using UnifiedBuildSlotsFn = std::function<int(const ReplyPassEvaluation &)>;

    class UnifiedTeamEvaluator
    {
    public:
        UnifiedTeamEvaluator(const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
                             const PropertyStockField & field, std::span<const PropertyFacts> properties,
                             CombatTerrain & local, const ReplyBoard & actingBoard, std::int64_t replayTariff,
                             std::int64_t & work, UnifiedHqRankFn hqRankFn, UnifiedBuildSlotsFn buildSlotsFn,
                             const UnifiedProductionModel * productionModel = nullptr);

        static int pawnTravelGrid(const CombatPawn & pawn);

        std::span<const TilePoint> enemyPositionsFor(int ki) const;

        int enemyPathDistance(int ki, TilePoint dest) const;

        int repairPathDistance(int ki, TilePoint dest) const;

        int unownedPropertyPathDistance(int ki, TilePoint dest) const;

        int unownedFactoryPathDistance(int ki, TilePoint dest) const;

        int combatObjectivePathDistance(int ki, TilePoint dest) const;

        int gridTileDistance(int gridId, TilePoint from, TilePoint goal, bool allowAdjacent = false) const;

        int tilePathDistance(int ki, TilePoint from, TilePoint goal, bool allowAdjacent = false) const;

        int gridMoveCostForUnit(int ki, TilePoint from, TilePoint to) const;

        bool enemyFactoryThreatensTile(TilePoint f, TilePoint tile) const;

        bool canSpawnReachAttack(int gridId, int moveBudget, int minRange, int maxRange,
                                 TilePoint from, TilePoint target) const;

        std::pair<int, int> unitRangePublic(int ki) const;

        int attackRingPathCostPublic(int ki, TilePoint from, TilePoint target, int minR, int maxR) const;

        int indirectScreenContactCount(int ownKi, TilePoint ownPos, std::span<const int> choices = {}) const
        {
            const auto [minR, maxR] = unitAttackRange(ownKi);
            if (minR <= 1 || maxR < minR || m_enemyPositions.empty() ||
                !m_local.grids.empty() &&
                    (ownPos.x < 0 || ownPos.x >= m_local.width || ownPos.y < 0 || ownPos.y >= m_local.height))
                return 0;
            const auto minEnemyManh = [&](TilePoint t) {
                int best = std::numeric_limits<int>::max();
                for (const TilePoint ep : m_enemyPositions)
                    best = std::min(best, manhattanDistance(t, ep));
                return best;
            };
            const int ownEnemyManh = minEnemyManh(ownPos);
            int bestCovered = 0;
            const auto scoreScreener = [&](int sKi, TilePoint sPos) {
                if (sKi == ownKi || sPos == INVALID_TILE || sPos == ownPos)
                    return;
                const auto [sMinR, sMaxR] = unitAttackRange(sKi);
                (void)sMaxR;
                if (sMinR > 1)
                    return;
                const int sEnemyManh = minEnemyManh(sPos);
                if (sEnemyManh >= ownEnemyManh)
                    return;
                for (const EntryDirection dir : ENTRY_DIRECTIONS)
                {
                    const StepVector step = entryStep(dir);
                    const TilePoint adj{sPos.x + step.dx, sPos.y + step.dy};
                    if (adj.x < 0 || adj.x >= m_local.width || adj.y < 0 || adj.y >= m_local.height)
                        continue;
                    if (minEnemyManh(adj) <= sEnemyManh && manhattanDistance(ownPos, adj) < minR)
                        return;
                }
                const int reachAhead = maxR - minR + 1;
                int covered = 0;
                for (int dy = -reachAhead; dy <= reachAhead; ++dy)
                {
                    for (int dx = -reachAhead; dx <= reachAhead; ++dx)
                    {
                        if (std::abs(dx) + std::abs(dy) == 0 || std::abs(dx) + std::abs(dy) > reachAhead)
                            continue;
                        const TilePoint q{sPos.x + dx, sPos.y + dy};
                        if (q.x < 0 || q.x >= m_local.width || q.y < 0 || q.y >= m_local.height)
                            continue;
                        if (minEnemyManh(q) > sEnemyManh)
                            continue;
                        const int d = manhattanDistance(ownPos, q);
                        if (d >= minR && d <= maxR)
                            ++covered;
                    }
                }
                bestCovered = std::max(bestCovered, covered);
            };
            if (!choices.empty())
            {
                std::vector<std::uint8_t> seenActor(m_local.units.size(), 0);
                for (const auto & m : m_problem.actors())
                {
                    if (m.knowledgeUnitIndex >= 0 && m.knowledgeUnitIndex < static_cast<int>(seenActor.size()))
                        seenActor[static_cast<std::size_t>(m.knowledgeUnitIndex)] = 1;
                    if (m.canonicalSlot < 0 || m.canonicalSlot >= static_cast<int>(choices.size()))
                        continue;
                    const int opt = choices[static_cast<std::size_t>(m.canonicalSlot)];
                    if (opt < 0 || opt >= static_cast<int>(m.candidates.size()))
                        continue;
                    scoreScreener(m.knowledgeUnitIndex, m.candidates[static_cast<std::size_t>(opt)].destination);
                }
                for (int uKi = 0; uKi < static_cast<int>(m_local.units.size()); ++uKi)
                {
                    if (seenActor[static_cast<std::size_t>(uKi)] != 0)
                        continue;
                    const auto & fp = m_local.units[static_cast<std::size_t>(uKi)];
                    if (fp.present && !fp.enemy)
                        scoreScreener(uKi, fp.tile);
                }
            }
            else
            {
                for (int uKi = 0; uKi < static_cast<int>(m_local.units.size()); ++uKi)
                {
                    const auto & fp = m_local.units[static_cast<std::size_t>(uKi)];
                    if (fp.present && !fp.enemy)
                        scoreScreener(uKi, fp.tile);
                }
            }
            return bestCovered;
        }

        bool isJammedInReachableBlindSpot(int ownKi, TilePoint ownPos, int minR) const;

        bool canExertZocOn(int blockerKi, int moverKi) const;

        bool canScreenCaptureTile(int helperKi, TilePoint pos, TilePoint capDest,
                                  std::span<const TilePoint> firingTiles = {}) const;

        std::int64_t computeGeodesicProgress(std::span<const int> choices) const;

        // Orders same-target attackers by counter safety and damage efficiency so heavy openers soften targets first.
        std::vector<CombatFireOrder> buildDefaultFireOrders(std::span<const int> choices) const;

        UnifiedTeamScore evaluate(std::span<const int> choices, const UnifiedTeamScore * pruneFloor = nullptr,
                                  ReplyPassEvaluation * economicOut = nullptr,
                                  std::span<const CombatFireOrder> fireOrdersOverride = {},
                                  bool useDefaultFireOrders = true);

        int evalCount() const { return m_evals; }
        int prunedCount() const { return m_pruned; }

        void refreshStockBaseline(const ReplyPassEvaluation & eval);

        MilliFunds testVacateRetaliationLoss(const ReplyPassEvaluation & eval) const;

        F1RetaliationResult testF1RetaliationResult(const ReplyPassEvaluation & eval) const;

        MilliFunds testProductionOpenSiteValue(const ReplyPassEvaluation & eval) const;

        int testEnemyPathDistance(int ki, TilePoint dest) const;

        MilliFunds testBreakableTerrainValue(const ReplyPassEvaluation & eval) const;

        std::vector<std::uint8_t> testComputeBlockedStockNodes(const ReplyPassEvaluation & eval) const;

        std::size_t testUnownedPropertyCount() const { return m_unownedProperties.size(); }
        std::size_t testEnemyFactoryCount() const { return m_enemyFactories.size(); }
        std::size_t testOwnCapturableCount() const { return m_ownCapturableProperties.size(); }

        std::pair<bool, MilliFunds> testOwnPropertyCaptureRisk(const ReplyPassEvaluation & eval) const;

        std::int64_t testComputeGeodesicProgress(std::span<const int> choices) const;

        MilliFunds breakableTerrainStake(int targetId) const;

        std::int32_t breakableTerrainHpSteps(int targetId) const;

        struct CaptureCoverActor
        {
            int ki{NO_UNIT};
            TilePoint pos{INVALID_TILE};
            double hp{0.0};
            int moveBudget{0};
            int strikeRange{1};
            int maxRange{1};
            MilliFunds cost{0};
            int rawMove{0};
        };

        bool canMoverReachFiringSquare(int moverKi, TilePoint from, TilePoint sq, int moveBudget,
                                       std::uint32_t occGen, TilePoint ignoredTile,
                                       const std::vector<std::uint32_t> & genGrid,
                                       const std::vector<int> & kiGrid) const;

        bool canFriendlyReachFiringSquare(int ki, TilePoint from, TilePoint sq, int moveBudget,
                                          std::uint32_t occEnemyGen) const;

        bool canEnemyReachFiringSquare(int eKi, TilePoint from, TilePoint sq, int moveBudget,
                                       std::uint32_t occFriendlyGen, TilePoint ignoredFriendlyTile = INVALID_TILE) const;

        bool canMoverReachAttackOccupied(int moverKi, TilePoint from, TilePoint target, int moveBudget,
                                         int minRange, int maxRange, std::uint32_t occGen, TilePoint ignoredTile,
                                         const std::vector<std::uint32_t> & genGrid,
                                         const std::vector<int> & kiGrid) const;

        bool canFriendlyReachAttackOccupied(int ki, TilePoint from, TilePoint target, int moveBudget,
                                            int minRange, int maxRange, std::uint32_t occEnemyGen) const;

        bool canEnemyReachAttackOccupied(int eKi, TilePoint from, TilePoint target, int moveBudget,
                                         int minRange, int maxRange, std::uint32_t occFriendlyGen,
                                         TilePoint ignoredFriendlyTile = INVALID_TILE) const;

        std::optional<double> defenderHpAfterCover(const CaptureCoverActor & fc, const CaptureCoverActor & d,
                                                   TilePoint tile, std::uint32_t occEnemyGen = 0,
                                                   std::uint32_t occFriendlyGen = 0) const;

        bool isCaptureTileCovered(const CaptureCoverActor & fc, const CaptureCoverActor & d, TilePoint tile,
                                  std::uint32_t occEnemyGen = 0, std::uint32_t occFriendlyGen = 0) const;

        bool testCaptureTileCovered(const CaptureCoverActor & fc, const CaptureCoverActor & d, TilePoint tile) const;

        std::vector<std::uint8_t> testBlockedStockNodes(const ReplyPassEvaluation & eval) const;

        MilliFunds testFollowupTrapLoss(const ReplyPassEvaluation & eval) const;

        std::optional<MilliFunds> testUnifiedRealizedMaterial(const ReplyPassEvaluation & eval) const;

        MilliFunds testF1RetaliationValue(const ReplyPassEvaluation & eval) const;

        MilliFunds testRepairValue(const ReplyPassEvaluation & eval) const;

        MilliFunds testForwardBoardCoverageValue(const ReplyPassEvaluation & eval) const;

        bool testIsJammedInReachableBlindSpot(int ownKi, TilePoint ownPos, int minR) const;

    private:
        MilliFunds propertyOneTurnSwingAt(TilePoint tile) const;

        void precomputeForwardEnemies();

        int attackRingPathCost(int ki, TilePoint from, TilePoint target, int minR, int maxR) const;

        TerrainDefenseRule bestMeleeFiringDefense(int ki, TilePoint from, TilePoint target,
                                                  int move, int maxCostBudget) const;

        // Pure Section 13 forward combat continuation Phi = D_ours - D_enemy over ETA < H (gamma = 1).
        MilliFunds computeForwardBoardCoverageValue(
            const ReplyPassEvaluation & eval,
            const F1RetaliationResult * f1Res = nullptr,
            std::span<const int> vacateKills = {}) const;
        bool isOnlyHitByDeterred(const ReplyPassEvaluation & eval, int targetKi,
                                 std::span<const int> deterredShooters, double & preDeterredHpOut) const;

        // Masks future nodes a lone capper cannot deliver without creating E0 lure or 1-HP chip cliffs.
        std::vector<std::uint8_t> computeBlockedStockNodes(
            const ReplyPassEvaluation & eval,
            std::span<const int> vacateKills = {},
            std::span<const int> deterredShooters = {}) const;

        MilliFunds cachedPostReplyStock(const ReplyPassEvaluation & eval,
                                        std::span<const int> vacateKills = {},
                                        std::span<const int> deterredShooters = {});

        // Credits exact marginal capture value for Enemy HQs, Com Towers, Airports, Ports, and forward Factories.
        MilliFunds computeStrategicCaptureBonus(const ReplyPassEvaluation & eval,
                                                std::span<const std::uint8_t> blocked,
                                                const ReplyStockFloor * floor = nullptr,
                                                std::span<const int> vacateKills = {},
                                                std::span<const int> deterredShooters = {}) const;

        // Exact marginal value of open friendly production sites plus E0 denial from blocking enemy production sites.
        MilliFunds computeProductionOpenSiteValue(const ReplyPassEvaluation & eval) const;

        double maxWeaponBaseDamageUncached(int attackerKi, int defenderKi, int range) const;

        double maxWeaponBaseDamage(int attackerKi, int defenderKi, int range) const;

        void precomputeWeaponTables();

        // Prices E0 direct retaliation unlocked when an enemy blocker vacates or dies adjacent to our unit.
        std::pair<MilliFunds, std::vector<int>> computeVacateRetaliationLoss(const ReplyPassEvaluation & eval) const;

        // Prices E1 follow-up loss for trapped units with conserved 1-action-per-enemy capacity.
        MilliFunds computeFollowupTrapLoss(const ReplyPassEvaluation & eval,
                                           std::span<const int> vacateKills = {}) const;

        MilliFunds computeEnemyDenial(const ReplyPassEvaluation & eval);

        std::pair<int, int> unitAttackRangeUncached(int ki) const;

        std::pair<int, int> unitAttackRange(int ki) const;

        static bool tileEnterable(const MobilityCostGrid & grid, TilePoint t);

        std::vector<std::int32_t> buildReversePathDistancesOnGrid(
            const MobilityCostGrid & grid, std::span<const TilePoint> targets, int minRange = 0, int maxRange = 0) const;

        std::vector<std::int32_t> buildReversePathDistances(
            int gridId, std::span<const TilePoint> targets, int minRange = 0, int maxRange = 0) const;

        int cachedSetPathDistance(const std::map<int, std::vector<std::int32_t>> & table,
                                  std::span<const TilePoint> fallbackTargets, int ki, TilePoint dest) const;

        bool canUnitReachAttack(int ki, TilePoint from, TilePoint target, int moveBudget,
                                int minRange, int maxRange) const;

        void computeBreakableProfiles();

        MilliFunds computeBreakableTerrainValue(const ReplyPassEvaluation & eval) const;

        void buildPathDistances();

        // Flags uninterruptible HQ capture starts and prices exact income swings on other own properties.
        std::pair<bool, MilliFunds> computeOwnPropertyCaptureRisk(
            const ReplyPassEvaluation & eval, std::span<const int> vacateKills = {}) const;

        int deployTurnsFromFactory(int ki, TilePoint tile) const;

        std::optional<MilliFunds> computeUnifiedRealizedMaterial(const ReplyPassEvaluation & eval) const;

        // Credits surviving second-line units ready to retaliate on F1 against E0 firing tiles.
        F1RetaliationResult computeF1RetaliationValue(
            const ReplyPassEvaluation & eval, std::span<const int> vacateKills = {}) const;

        // Prices embodied forward-deployment capital and smooth 1..9 HP repair tempo directly in MilliFunds.
        MilliFunds computeRepairValue(const ReplyPassEvaluation & eval,
                                      std::span<const int> vacateKills = {},
                                      std::span<const int> deterredShooters = {}) const;

        struct OwnCapturableProperty
        {
            TilePoint tile{INVALID_TILE};
            PropertyIncome income{};
            bool isHq{false};
        };
        struct ForwardEnemySummary
        {
            int ki{NO_UNIT};
            TilePoint initialTile{INVALID_TILE};
            MilliFunds cost{0};
            int move{0};
            int minRange{1};
            int maxRange{1};
            std::int32_t directAttackSlots{UNIFIED_PROD_OPEN_ADJACENT_SLOTS};
            bool canMoveAndFire{true};
            bool screenableByDirect{true};
            bool capper{false};
            MilliFunds fullFirstStrikeThreat{0};
            MilliFunds fullCapThreatPerTurn{0};
        };

        struct PathDistKey
        {
            int gridId{0};
            int x{0};
            int y{0};
            int minR{0};
            int maxR{0};

            friend bool operator==(const PathDistKey &, const PathDistKey &) = default;
        };
        struct PathDistKeyHash
        {
            static constexpr std::size_t HASH_MIX = 0x9e3779b97f4a7c15ULL;
            static constexpr std::size_t HASH_LEFT_SHIFT = 6;

            std::size_t operator()(const PathDistKey & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(k.gridId) * HASH_MIX;
                h ^= static_cast<std::size_t>(k.x) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                h ^= static_cast<std::size_t>(k.y) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                h ^= static_cast<std::size_t>(k.minR) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                h ^= static_cast<std::size_t>(k.maxR) + HASH_MIX + (h << HASH_LEFT_SHIFT) + (h >> 2);
                return h;
            }
        };

        std::uint32_t nextBfsGen() const;

        std::uint32_t nextOccFriendlyGen() const;

        std::uint32_t nextOccEnemyGen() const;

        bool hasAdjacentZoc(TilePoint cur, int moverKi, std::uint32_t occGen, TilePoint ignoredTile,
                            const std::vector<std::uint32_t> & genGrid,
                            const std::vector<int> & kiGrid) const;

        bool boundedCanReachGoalOnOcc(int moverKi, int gridId, TilePoint start, TilePoint goal, int moveBudget,
                                      std::uint32_t occGen, TilePoint ignoredTile,
                                      const std::vector<std::uint32_t> & genGrid,
                                      const std::vector<int> & kiGrid) const;

        bool boundedCanReachGoal(int moverKi, int gridId, TilePoint start, TilePoint goal, int moveBudget,
                                 std::uint32_t occFriendlyGen, TilePoint ignoredFriendly, bool allowCollision) const;

        void collectReachableEscapeTiles(int victimKi, int gridId, TilePoint start, int moveBudget,
                                         bool ignoresCollision, std::uint32_t occEnemyGen) const;

        void collectEnemyEscapeTiles(int enemyKi, int gridId, TilePoint start, int moveBudget,
                                     bool ignoresCollision, std::uint32_t occFriendlyGen,
                                     std::uint32_t occEnemyGen) const;

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
        UnifiedHqRankFn m_hqRankFn;
        UnifiedBuildSlotsFn m_buildSlotsFn;
        const UnifiedProductionModel * m_productionModel{nullptr};
        std::vector<std::pair<TilePoint, OwnerSign>> m_strategicBonusTiles;
        mutable std::map<std::uint64_t, MilliFunds> m_openSiteValueCache;
        int m_maxDist{1};
        std::size_t m_unitCount{0};
        std::size_t m_cellCount{0};
        std::vector<std::pair<int, int>> m_unitRanges;
        std::vector<double> m_baseDmgRange1;
        std::vector<double> m_baseDmgMinRange;
        int m_sampleCapperKi{NO_UNIT};
        std::vector<TilePoint> m_enemyPositions;
        std::vector<std::pair<int, TilePoint>> m_enemyUnits;
        std::vector<TilePoint> m_unownedProperties;
        std::vector<TilePoint> m_enemyOwnedProperties;
        std::vector<TilePoint> m_unownedNeutralProperties;
        std::vector<TilePoint> m_unownedFactories;
        std::vector<TilePoint> m_deliverableNeutralProperties;
        std::vector<TilePoint> m_enemyFactories;
        std::vector<TilePoint> m_friendlyFactories;
        std::vector<TilePoint> m_friendlyRepairSites;
        std::vector<OwnCapturableProperty> m_ownCapturableProperties;
        std::vector<ForwardEnemySummary> m_forwardEnemies;
        struct BreakableProfile
        {
            std::int32_t targetId{NO_UNIT};
            TilePoint tile{INVALID_TILE};
            std::int32_t initialHpSteps{UNIT_HP_STEPS};
            std::int32_t maxSingleTurnSteps{1};
            std::vector<MilliFunds> surplusByBreachDelay;
        };

        std::map<int, std::vector<TilePoint>> m_targetableEnemiesByKi;
        std::map<std::tuple<int, int, int, std::vector<int>>, std::vector<std::int32_t>> m_enemyPathDistByTargetSet;
        std::map<int, const std::vector<std::int32_t> *> m_enemyPathDistByKi;
        std::map<int, std::vector<std::int32_t>> m_factoryPathDistByGrid;
        std::map<int, std::vector<std::int32_t>> m_repairPathDistByGrid;
        std::map<std::pair<int, std::vector<std::uint8_t>>, std::vector<std::int32_t>> m_repairDistCache;
        std::map<std::vector<std::uint8_t>, std::vector<std::uint8_t>> m_repairCellMaskCache;
        std::vector<const std::vector<std::int32_t> *> m_repairPathDistByKi;
        std::vector<const std::vector<std::uint8_t> *> m_repairSiteMaskByKi;
        std::vector<std::uint8_t> m_actorHasSafeCandidate;
        std::vector<std::uint8_t> m_actorHasSafeStayCapture;
        std::map<int, std::vector<std::int32_t>> m_unownedPropPathDistByGrid;
        std::map<int, std::vector<std::int32_t>> m_enemyPropPathDistByGrid;
        std::map<int, std::vector<std::int32_t>> m_openPropPathDistByGrid;
        std::map<int, std::vector<std::int32_t>> m_unownedFactoryPathDistByGrid;
        std::map<int, BreakableProfile> m_breakableProfiles;
        std::vector<TilePoint> m_valuableBreakables;
        mutable std::unordered_map<PathDistKey, std::vector<std::int32_t>, PathDistKeyHash> m_tilePathDistCache;
        mutable std::unordered_map<PathDistKey, std::vector<std::int32_t>, PathDistKeyHash> m_indirectTilePathCache;
        mutable std::vector<std::int32_t> m_bfsDist;
        mutable std::vector<std::uint32_t> m_bfsGen;
        mutable std::uint32_t m_curBfsGen{0};
        mutable std::vector<std::pair<std::int32_t, TilePoint>> m_bfsHeap;
        mutable std::vector<TilePoint> m_escapeTilesScratch;
        mutable std::vector<std::uint32_t> m_occFriendlyGen;
        mutable std::vector<int> m_occFriendlyKi;
        mutable std::uint32_t m_curOccFriendlyGen{0};
        mutable std::vector<std::uint32_t> m_occEnemyGen;
        mutable std::vector<int> m_occEnemyKi;
        mutable std::uint32_t m_curOccEnemyGen{0};
        std::map<std::pair<ContinuationKey, std::vector<std::uint8_t>>, MilliFunds> m_seqStockCache;
        int m_evals{0};
        int m_pruned{0};
        const char * m_profilePath{std::getenv("COW_UNIFIED_PROFILE")};
        bool m_profileEnabled{m_profilePath != nullptr && m_profilePath[0] != '\0'};
        mutable std::int64_t m_profScored{0};
        mutable std::int64_t m_profReused{0};
        mutable std::int64_t m_profReplyNs{0};
        mutable std::int64_t m_profHqNs{0};
        mutable std::int64_t m_profRealizedNs{0};
        mutable std::int64_t m_profVacateNs{0};
        mutable std::int64_t m_profF1Ns{0};
        mutable std::int64_t m_profStockNs{0};
        mutable std::int64_t m_profFwdCovNs{0};
        mutable std::int64_t m_profDenialNs{0};
        mutable std::int64_t m_profOwnRiskNs{0};
        mutable std::int64_t m_profTrapNs{0};
        mutable std::int64_t m_profFactoryNs{0};
        mutable std::int64_t m_profRepairBrkNs{0};
        struct FastCache;
        mutable std::shared_ptr<FastCache> m_fast;

    public:
        ~UnifiedTeamEvaluator();
    };
}
