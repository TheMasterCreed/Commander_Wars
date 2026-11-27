#pragma once

#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include <algorithm>
#include <cmath>
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
    inline constexpr double UNIFIED_PROD_FULL_HP = 10.0;
    inline constexpr double UNIFIED_PROD_HP_PERCENT_PER_STEP = 10.0;
    inline constexpr double UNIFIED_PROD_PERCENT_SCALE = 100.0;
    inline constexpr std::int32_t UNIFIED_PROD_DEFAULT_HORIZON = 10;
    inline constexpr std::int32_t UNIFIED_PROD_MIN_POST_CAP_TURNS = 4;
    inline constexpr std::int32_t UNIFIED_PROD_MAX_MAP_HORIZON = 18;
    inline constexpr std::int32_t UNIFIED_PROD_DEFAULT_CAP_POINTS = 20;
    inline constexpr std::size_t UNIFIED_PROD_EXACT_SITE_LIMIT = 4;
    inline constexpr std::size_t UNIFIED_PROD_PARETO_PER_SITE = 6;
    inline constexpr std::int32_t UNIFIED_PROD_UNREACHABLE_TURNS = 1000000;
    inline constexpr std::int32_t UNIFIED_PROD_DEFAULT_SITE_DEFENSE_STARS = 3;
    inline constexpr double UNIFIED_PROD_DEFENSE_PER_STAR_PCT = 10.0;
    inline constexpr double UNIFIED_PROD_MIN_DEFENSE_FACTOR = 0.1;
    inline constexpr std::int32_t UNIFIED_PROD_FUTURE_WAVES = 1;
    inline constexpr std::int32_t UNIFIED_PROD_OPEN_ADJACENT_SLOTS = 4;
    inline constexpr std::int32_t UNIFIED_PROD_REPAIR_STEPS = 2;

    struct ProductionUnitTemplate
    {
        QString unitId;
        MilliFunds cost{0};
        std::int32_t movementPoints{0};
        std::int32_t minRange{1};
        std::int32_t maxRange{1};
        std::int32_t maxAmmo{0};
        bool canMoveAndFire{true};
        bool canCapture{false};
        std::int32_t captureRate{0};
        bool hasWeapons{false};
        bool isTransporter{false};
        bool bypassesGroundDefense{false};
        std::int32_t unitType{0};
        int gridId{-1};
        // Indexed by enemyIndex in ProductionModelInput::enemies; negative means cannot hurt.
        std::vector<double> baseDamageVsEnemy;
        std::vector<double> returnDamageFromEnemy;
        std::vector<double> enemyFirstStrikeVsMe;
        std::vector<double> counterDamageWhenStruck;
        // Indexed by templateIndex in ProductionModelInput::templates; negative means cannot hurt.
        std::vector<double> baseDamageVsTemplate;
    };

    struct ProductionEnemyUnit
    {
        int knowledgeUnitIndex{NO_UNIT};
        TilePoint tile{INVALID_TILE};
        int gridId{-1};
        MilliFunds replacementCost{0};
        double hp{UNIFIED_PROD_FULL_HP};
        std::int32_t movementPoints{0};
        std::int32_t minRange{1};
        std::int32_t maxRange{1};
        bool canMoveAndFire{true};
        bool canCapture{false};
        std::int32_t captureRate{0};
        MilliFunds perTurnThreat{0};
        std::int32_t unitType{0};
        std::int32_t spawnDelayTurns{0};
        bool bypassesGroundScreen{false};
        bool isProjected{false};
        double initialFriendlyCoverage{0.0};
        std::int32_t directAttackSlots{UNIFIED_PROD_OPEN_ADJACENT_SLOTS};
    };

    struct UnifiedProductionSite
    {
        TilePoint tile{INVALID_TILE};
        OwnerSign owner{OwnerSign::Neutral};
        QString buildingId;
        std::vector<int> buildableTemplateIndices;
        MilliFunds ownerAvailableFunds{0};
        std::int32_t defenseStars{UNIFIED_PROD_DEFAULT_SITE_DEFENSE_STARS};
        // Indexed by friendly knowledgeUnitIndex when populated for enemy sites.
        std::vector<int> enemyCounterTemplateIndices;
        // Indexed by enemyIndex when populated on own production sites for spawn-pad E0 survival.
        std::vector<std::uint8_t> enemyCanStrikeInE0;
        std::vector<std::uint8_t> enemyCapperCanStepInE0;
    };

    struct ProductionCapturableProperty
    {
        TilePoint tile{INVALID_TILE};
        MilliFunds income{0};
        OwnerSign owner{OwnerSign::Neutral};
        std::int32_t claimedTurn{UNIFIED_PROD_DEFAULT_HORIZON};
        bool blocked{false};
        std::int32_t offensiveBonus{0};
        std::int32_t defensiveBonus{0};
        std::int32_t powerChargeBonus{0};
        MilliFunds hqEliminationValue{0};
    };

    struct EnemySiteCounterThreat
    {
        int gridId{-1};
        std::int32_t movementPoints{0};
        std::int32_t minRange{1};
        std::int32_t maxRange{1};
        bool canMoveAndFire{true};
        MilliFunds cost{0};
        double baseDamageVsFriendly{0.0};
        double friendlyReturnDamage{0.0};
    };

    struct ProductionModelInput
    {
        std::int32_t width{0};
        std::int32_t height{0};
        std::int32_t horizonTurns{UNIFIED_PROD_DEFAULT_HORIZON};
        std::int32_t capturePointsToCapture{UNIFIED_PROD_DEFAULT_CAP_POINTS};
        MilliFunds currentFunds{0};
        MilliFunds nextTurnIncome{0};
        MilliFunds nextTurnRepairBill{0};
        MilliFunds friendlyDirectCapital{0};
        MilliFunds enemyDirectCapital{0};
        MilliFunds friendlyAntiAirCapital{0};
        MilliFunds enemyAirCapital{0};
        MilliFunds totalFriendlyCombatCapital{0};
        MilliFunds totalEnemyCombatCapital{0};
        std::int32_t friendlyScreenBodies{0};
        std::int32_t enemyDirectCount{0};
        std::int32_t friendlyCombatUnitCount{0};
        std::int32_t friendlyDirectVehicleCount{0};
        std::int32_t friendlyIndirectCount{0};
        std::int32_t friendlyCapperCount{0};
        std::int32_t totalFrontDirectSlots{0};
        std::vector<MobilityCostGrid> grids;
        std::vector<ProductionUnitTemplate> templates;
        std::vector<ProductionEnemyUnit> enemies;
        std::vector<UnifiedProductionSite> sites;
        std::vector<ProductionCapturableProperty> properties;
        // Keyed by (enemySite.x, enemySite.y, friendlyKnowledgeUnitIndex) for exact enemy-spawn box-in checks.
        std::map<std::tuple<std::int32_t, std::int32_t, int>, EnemySiteCounterThreat> enemySiteCounters;
        // Exact per-turn denial value (MilliFunds) when a surviving friendly unit blocks an enemy production site.
        std::map<std::pair<std::int32_t, std::int32_t>, MilliFunds> enemySiteBlockValue;
        // Exact per-horizon loss (MilliFunds) when an enemy captures our own Com Tower, Power Plant, or production site.
        std::map<std::pair<std::int32_t, std::int32_t>, MilliFunds> ownStrategicLoss;
    };

    struct ProductionBuildOrder
    {
        TilePoint tile{INVALID_TILE};
        QString unitId;
        int templateIndex{-1};
        MilliFunds cost{0};
        MilliFunds utility{0};
        MilliFunds capperGain{0};
        MilliFunds combatGain{0};
        std::int32_t arrivalTurns{UNIFIED_PROD_UNREACHABLE_TURNS};
    };

    struct ProductionPlan
    {
        std::vector<ProductionBuildOrder> orders;
        MilliFunds todayUtility{0};
        MilliFunds tomorrowUtility{0};
        MilliFunds totalValue{0};
        MilliFunds saveOnlyValue{0};
        MilliFunds spentFunds{0};
        MilliFunds remainingFunds{0};
        bool choseSave{false};
    };

    class UnifiedProductionModel
    {
    public:
        UnifiedProductionModel() = default;

        explicit UnifiedProductionModel(ProductionModelInput input)
            : m_input(std::move(input))
        {
            precompute();
        }

        bool valid() const
        {
            return m_input.width > 0 && m_input.height > 0 && m_input.horizonTurns > 0;
        }

        const ProductionModelInput & input() const { return m_input; }

        MilliFunds currentFunds() const { return m_input.currentFunds; }
        MilliFunds nextTurnIncome() const { return m_input.nextTurnIncome; }

        std::optional<EnemySiteCounterThreat> bestEnemyCounterFromSite(TilePoint enemySite, int friendlyKi) const
        {
            const auto it = m_input.enemySiteCounters.find({enemySite.x, enemySite.y, friendlyKi});
            if (it == m_input.enemySiteCounters.end())
                return std::nullopt;
            return it->second;
        }

        MilliFunds enemySiteBlockDenial(TilePoint enemySite) const
        {
            const auto it = m_input.enemySiteBlockValue.find({enemySite.x, enemySite.y});
            if (it == m_input.enemySiteBlockValue.end())
                return 0;
            return std::max<MilliFunds>(0, it->second);
        }

        // Exact strategic loss if an enemy captures our own Com Tower, Power Plant, Factory, Airport, or Port.
        MilliFunds ownStrategicCaptureLoss(TilePoint ownTile, std::int32_t horizon) const
        {
            const auto it = m_input.ownStrategicLoss.find({ownTile.x, ownTile.y});
            if (it == m_input.ownStrategicLoss.end() || it->second <= 0 || horizon <= 1)
                return 0;
            return (it->second * std::max<std::int32_t>(1, horizon - 1)) / std::max<std::int32_t>(1, horizon);
        }

        // Exact strategic denial credit when interrupting an enemy capper on a Com Tower, Power Plant, or production site.
        MilliFunds enemyCaptureInterruptStrategicValue(TilePoint tile, std::int32_t delayTurns,
                                                       std::int32_t horizon) const
        {
            if (!valid() || delayTurns <= 0 || horizon <= 0)
                return 0;
            const std::int32_t clampedDelay = std::min(delayTurns, horizon);
            if (const auto it = m_input.ownStrategicLoss.find({tile.x, tile.y});
                it != m_input.ownStrategicLoss.end() && it->second > 0)
            {
                return (it->second * clampedDelay) / horizon;
            }
            const MilliFunds fullBonus = combatBonusCaptureValue(tile, 0, horizon) +
                siteCaptureProductionSurplus(tile, 0, horizon);
            if (fullBonus <= 0)
                return 0;
            return (fullBonus * clampedDelay) / horizon;
        }

        ProductionPlan planBuilds(std::span<const TilePoint> openFriendlySites,
                                  MilliFunds spendableFunds,
                                  std::span<const std::int32_t> claimedTurnOverride = {}) const
        {
            ProductionPlan result;
            result.remainingFunds = std::max<MilliFunds>(0, spendableFunds);
            if (!valid() || m_input.templates.empty())
                return result;

            std::vector<int> openSiteIndices;
            openSiteIndices.reserve(openFriendlySites.size());
            for (const TilePoint t : openFriendlySites)
            {
                const int idx = findSiteIndex(t, OwnerSign::Ours);
                if (idx >= 0 && std::ranges::find(openSiteIndices, idx) == openSiteIndices.end())
                    openSiteIndices.push_back(idx);
            }
            std::vector<int> allOwnSiteIndices;
            for (int i = 0; i < static_cast<int>(m_input.sites.size()); ++i)
            {
                if (m_input.sites[static_cast<std::size_t>(i)].owner == OwnerSign::Ours)
                    allOwnSiteIndices.push_back(i);
            }

            const std::vector<std::int32_t> baseClaimed = buildClaimedTurns(claimedTurnOverride);
            const std::vector<double> baseCoverage = buildInitialCoverage();

            MilliFunds saveOnlyTomorrow = evaluateTomorrowBest(
                allOwnSiteIndices,
                std::max<MilliFunds>(0, spendableFunds + m_input.nextTurnIncome - m_input.nextTurnRepairBill),
                baseCoverage, baseClaimed, m_input.friendlyDirectCapital, effectiveDirectVehicles(),
                m_input.friendlyCombatUnitCount, effectiveScreenBodies(), m_input.friendlyIndirectCount,
                m_input.friendlyCapperCount);
            result.saveOnlyValue = saveOnlyTomorrow;
            result.tomorrowUtility = saveOnlyTomorrow;
            result.totalValue = saveOnlyTomorrow;
            result.choseSave = true;

            if (openSiteIndices.empty() || spendableFunds <= 0)
                return result;

            // Sort open sites by fastest combat/capture arrival, breaking ties in favor of ground sites over air-only sites.
            std::ranges::sort(openSiteIndices, [&](int a, int b) {
                return std::tuple{siteMinArrival(a), siteBuildsOnlyAir(a) ? 1 : 0} <
                    std::tuple{siteMinArrival(b), siteBuildsOnlyAir(b) ? 1 : 0};
            });

            if (openSiteIndices.size() <= UNIFIED_PROD_EXACT_SITE_LIMIT)
            {
                searchExactToday(openSiteIndices, allOwnSiteIndices, spendableFunds, baseCoverage, baseClaimed, result);
            }
            else
            {
                searchGreedyWithBigAlternatives(
                    openSiteIndices, allOwnSiteIndices, spendableFunds, baseCoverage, baseClaimed, result);
            }

            result.choseSave = result.orders.empty() && result.saveOnlyValue > 0;
            return result;
        }

        MilliFunds marginalOpenSitesValue(std::span<const TilePoint> openFriendlySites,
                                          MilliFunds spendableFunds,
                                          std::span<const std::int32_t> claimedTurnOverride = {}) const
        {
            if (openFriendlySites.empty() || spendableFunds <= 0 || !valid())
                return 0;
            const ProductionPlan plan = planBuilds(openFriendlySites, spendableFunds, claimedTurnOverride);
            return std::max<MilliFunds>(0, plan.totalValue - plan.saveOnlyValue);
        }

        // Exact horizon utility of placing a free unit of templateIndex at siteTile (evaluated at its true replacementCost).
        MilliFunds zeroCostSpawnHorizonValue(TilePoint siteTile, int templateIndex) const
        {
            if (!valid() || templateIndex < 0 || templateIndex >= static_cast<int>(m_input.templates.size()))
                return 0;
            const int siteIdx = findSiteIndex(siteTile, OwnerSign::Ours);
            if (siteIdx < 0)
                return 0;
            const auto & tmpl = m_input.templates[static_cast<std::size_t>(templateIndex)];
            if (tmpl.cost <= 0)
                return 0;
            auto claims = buildClaimedTurns({});
            const auto cov = buildInitialCoverage();
            const auto sc = scoreSiteTemplate(
                siteIdx, templateIndex, 0, cov, claims, m_input.friendlyDirectCapital,
                effectiveDirectVehicles(), m_input.friendlyCombatUnitCount, effectiveScreenBodies(),
                m_input.friendlyIndirectCount, m_input.friendlyCapperCount, false);
            return tmpl.cost + sc.utility;
        }

        // Exact marginal production surplus from capturing a neutral or enemy Factory, Airport, or Port.
        MilliFunds siteCaptureProductionSurplus(TilePoint siteTile, std::int32_t captureTurn,
                                                std::int32_t horizon) const
        {
            if (!valid() || captureTurn < 0 || captureTurn >= horizon - 1)
                return 0;
            int siteIdx = -1;
            for (int i = 0; i < static_cast<int>(m_input.sites.size()); ++i)
            {
                if (m_input.sites[static_cast<std::size_t>(i)].tile == siteTile &&
                    m_input.sites[static_cast<std::size_t>(i)].owner != OwnerSign::Ours)
                {
                    siteIdx = i;
                    break;
                }
            }
            if (siteIdx < 0)
                return 0;
            const auto it = m_captureSitePerTurnSurplus.find(siteIdx);
            if (it == m_captureSitePerTurnSurplus.end() || it->second <= 0)
                return 0;
            const std::int32_t activeProductionTurns = std::max<std::int32_t>(0, horizon - captureTurn - 1);
            // Scale by remaining horizon share so capturing earlier earns proportionally more surplus.
            return (it->second * activeProductionTurns) / std::max<std::int32_t>(1, horizon);
        }

        // Exact combat-bonus capture value for Com Towers and any modded attack/defense buff buildings.
        MilliFunds combatBonusCaptureValue(TilePoint tile, std::int32_t ownTurn, std::int32_t horizon) const
        {
            if (!valid() || ownTurn < 0 || ownTurn >= horizon)
                return 0;
            for (const auto & prop : m_input.properties)
            {
                if (prop.tile != tile)
                    continue;
                const std::int32_t remTurns = std::max<std::int32_t>(0, horizon - ownTurn);
                MilliFunds hqGain = 0;
                if (prop.hqEliminationValue > 0)
                    hqGain = (prop.hqEliminationValue * remTurns) / std::max<std::int32_t>(1, horizon);
                const std::int32_t bonusPct =
                    std::max<std::int32_t>(0, prop.offensiveBonus) +
                    std::max<std::int32_t>(0, prop.defensiveBonus) +
                    std::max<std::int32_t>(0, prop.powerChargeBonus) / 2;
                if (bonusPct <= 0)
                    return hqGain;
                const MilliFunds futureArmyScale = m_input.nextTurnIncome * std::max<std::int32_t>(1, horizon / 2);
                const MilliFunds ourArmy = m_input.totalFriendlyCombatCapital + futureArmyScale;
                const MilliFunds ourGain = static_cast<MilliFunds>(std::llround(
                    (static_cast<double>(ourArmy) * (static_cast<double>(bonusPct) / UNIFIED_PROD_PERCENT_SCALE)) *
                    (static_cast<double>(remTurns) / static_cast<double>(std::max<std::int32_t>(1, horizon)))));
                MilliFunds enemySwing = 0;
                if (prop.owner == OwnerSign::Enemy)
                {
                    const MilliFunds enemyArmy = m_input.totalEnemyCombatCapital + futureArmyScale;
                    enemySwing = static_cast<MilliFunds>(std::llround(
                        (static_cast<double>(enemyArmy) * (static_cast<double>(bonusPct) / UNIFIED_PROD_PERCENT_SCALE)) *
                        (static_cast<double>(remTurns) / static_cast<double>(std::max<std::int32_t>(1, horizon)))));
                }
                return hqGain + ourGain + enemySwing;
            }
            return 0;
        }

        // Lists all unowned strategic bonus sites (HQ, Factories, Airports, Ports, Com Towers) for capture valuation.
        std::vector<std::pair<TilePoint, OwnerSign>> strategicCaptureTiles() const
        {
            std::vector<std::pair<TilePoint, OwnerSign>> out;
            for (const auto & prop : m_input.properties)
            {
                const bool hasCombatBonus = prop.hqEliminationValue > 0 ||
                    prop.offensiveBonus > 0 || prop.defensiveBonus > 0 || prop.powerChargeBonus > 0;
                const bool hasProdSurplus = siteCaptureProductionSurplus(prop.tile, 0, m_input.horizonTurns) > 0;
                if (hasCombatBonus || hasProdSurplus)
                    out.emplace_back(prop.tile, prop.owner);
            }
            return out;
        }

        static double firstStrikeNetTrade(const ProductionUnitTemplate & unit, const ProductionEnemyUnit & enemy,
                                          double baseDmg, double returnDmg, std::int32_t fireDist = 1)
        {
            if (baseDmg <= 0.0 || enemy.hp <= 0.0)
                return 0.0;
            const double dealtHp = std::min(enemy.hp, baseDmg / UNIFIED_PROD_HP_PERCENT_PER_STEP);
            const double afterEnemyHp = std::max(0.0, enemy.hp - dealtHp);
            const double enemyCapLoss =
                (static_cast<double>(enemy.replacementCost) * dealtHp) / UNIFIED_PROD_FULL_HP;
            double ourCounterLoss = 0.0;
            if (fireDist <= 1 && unit.canMoveAndFire && enemy.canMoveAndFire && enemy.minRange <= 1 &&
                afterEnemyHp > 0.0 && returnDmg > 0.0)
            {
                const double scaledRet = returnDmg * (afterEnemyHp / UNIFIED_PROD_FULL_HP);
                const double retHp = std::min(UNIFIED_PROD_FULL_HP, scaledRet / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                ourCounterLoss = (static_cast<double>(unit.cost) * retHp) / UNIFIED_PROD_FULL_HP;
            }
            return std::max(0.0, enemyCapLoss - ourCounterLoss);
        }

        static double enemyFirstStrikeNetThreat(const ProductionUnitTemplate & unit, const ProductionEnemyUnit & enemy,
                                                double enemyStrikeDmg, double ourCounterDmg,
                                                std::int32_t fireDist = 1)
        {
            if (enemyStrikeDmg <= 0.0 || enemy.hp <= 0.0)
                return 0.0;
            const double scaledStrike = enemyStrikeDmg * (enemy.hp / UNIFIED_PROD_FULL_HP);
            const double dealtHp = std::min(UNIFIED_PROD_FULL_HP, scaledStrike / UNIFIED_PROD_HP_PERCENT_PER_STEP);
            const double ourAfterHp = std::max(0.0, UNIFIED_PROD_FULL_HP - dealtHp);
            const double ourLoss = (static_cast<double>(unit.cost) * dealtHp) / UNIFIED_PROD_FULL_HP;
            double enemyCounterLoss = 0.0;
            if (fireDist <= 1 && enemy.canMoveAndFire && unit.canMoveAndFire && unit.minRange <= 1 &&
                ourAfterHp > 0.0 && ourCounterDmg > 0.0)
            {
                const double scaledCounter = ourCounterDmg * (ourAfterHp / UNIFIED_PROD_FULL_HP);
                const double counterHp = std::min(enemy.hp, scaledCounter / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                enemyCounterLoss = (static_cast<double>(enemy.replacementCost) * counterHp) / UNIFIED_PROD_FULL_HP;
            }
            return std::max(0.0, ourLoss - enemyCounterLoss);
        }

    private:
        struct CandidateScore
        {
            int templateIndex{-1};
            MilliFunds cost{0};
            MilliFunds utility{0};
            MilliFunds capperGain{0};
            MilliFunds combatGain{0};
            MilliFunds screenGain{0};
            std::int32_t arrivalTurns{UNIFIED_PROD_UNREACHABLE_TURNS};
            std::vector<std::pair<std::size_t, double>> claimedCoverage;
        };

        struct CombatEvalResult
        {
            MilliFunds gain{0};
            std::int32_t firstFireTurn{UNIFIED_PROD_UNREACHABLE_TURNS};
            std::vector<std::pair<std::size_t, double>> claimedCoverage;
        };

        static bool tileEnterable(const MobilityCostGrid & grid, TilePoint t)
        {
            if (!grid.onGrid(t.x, t.y))
                return false;
            for (const EntryDirection dir : ENTRY_DIRECTIONS)
            {
                if (grid.cost(t.x, t.y, dir) != IMPASSABLE_COST)
                    return true;
            }
            return false;
        }

        std::vector<std::int32_t> buildReverseDistances(int gridId, std::span<const TilePoint> targets,
                                                        std::int32_t minRange, std::int32_t maxRange) const
        {
            const std::size_t cells =
                static_cast<std::size_t>(m_input.width) * static_cast<std::size_t>(m_input.height);
            std::vector<std::int32_t> dist(cells, std::numeric_limits<std::int32_t>::max());
            if (gridId < 0 || gridId >= static_cast<int>(m_input.grids.size()) || targets.empty())
                return dist;
            const auto & grid = m_input.grids[static_cast<std::size_t>(gridId)];
            const auto idxOf = [&](TilePoint t) {
                return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_input.width) +
                    static_cast<std::size_t>(t.x);
            };
            using Node = std::pair<std::int32_t, TilePoint>;
            const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
            std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
            for (const TilePoint t : targets)
            {
                if (!grid.onGrid(t.x, t.y))
                    continue;
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
                    const int x1 = std::min(m_input.width - 1, t.x + maxRange);
                    const int y0 = std::max(0, t.y - maxRange);
                    const int y1 = std::min(m_input.height - 1, t.y + maxRange);
                    for (int y = y0; y <= y1; ++y)
                    {
                        for (int x = x0; x <= x1; ++x)
                        {
                            const TilePoint p{x, y};
                            const int d = manhattanDistance(p, t);
                            if (d < minRange || d > maxRange || !tileEnterable(grid, p))
                                continue;
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
                if (d > dist[idxOf(cur)])
                    continue;
                for (const EntryDirection dir : ENTRY_DIRECTIONS)
                {
                    const StepVector step = entryStep(dir);
                    const TilePoint prev{cur.x - step.dx, cur.y - step.dy};
                    if (!grid.onGrid(prev.x, prev.y))
                        continue;
                    const std::int32_t stepCost = grid.cost(cur.x, cur.y, dir);
                    if (stepCost == IMPASSABLE_COST)
                        continue;
                    const std::int32_t nextDist = d + stepCost;
                    if (nextDist < dist[idxOf(prev)])
                    {
                        dist[idxOf(prev)] = nextDist;
                        pq.push({nextDist, prev});
                    }
                }
            }
            return dist;
        }

        std::vector<std::int32_t> buildForwardDistances(int gridId, TilePoint origin) const
        {
            const std::size_t cells =
                static_cast<std::size_t>(m_input.width) * static_cast<std::size_t>(m_input.height);
            std::vector<std::int32_t> dist(cells, std::numeric_limits<std::int32_t>::max());
            if (gridId < 0 || gridId >= static_cast<int>(m_input.grids.size()))
                return dist;
            const auto & grid = m_input.grids[static_cast<std::size_t>(gridId)];
            if (!grid.onGrid(origin.x, origin.y))
                return dist;
            const auto idxOf = [&](TilePoint t) {
                return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(m_input.width) +
                    static_cast<std::size_t>(t.x);
            };
            using Node = std::pair<std::int32_t, TilePoint>;
            const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
            std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
            dist[idxOf(origin)] = 0;
            pq.push({0, origin});
            while (!pq.empty())
            {
                const auto [d, cur] = pq.top();
                pq.pop();
                if (d > dist[idxOf(cur)])
                    continue;
                for (const EntryDirection dir : ENTRY_DIRECTIONS)
                {
                    const StepVector step = entryStep(dir);
                    const TilePoint next{cur.x + step.dx, cur.y + step.dy};
                    if (!grid.onGrid(next.x, next.y))
                        continue;
                    const std::int32_t stepCost = grid.cost(next.x, next.y, dir);
                    if (stepCost == IMPASSABLE_COST)
                        continue;
                    const std::int32_t nextDist = d + stepCost;
                    if (nextDist < dist[idxOf(next)])
                    {
                        dist[idxOf(next)] = nextDist;
                        pq.push({nextDist, next});
                    }
                }
            }
            return dist;
        }

        void precompute()
        {
            if (!valid())
                return;
            const std::size_t siteCount = m_input.sites.size();
            const std::size_t tmplCount = m_input.templates.size();
            const std::size_t propCount = m_input.properties.size();
            m_combatMoveTurns.assign(siteCount, std::vector<std::int32_t>(tmplCount, UNIFIED_PROD_UNREACHABLE_TURNS));

            std::int32_t computedSlots = 0;
            m_liveGroundEnemyCount = 0;
            for (const auto & enemy : m_input.enemies)
            {
                if (enemy.hp <= 0.0 || enemy.bypassesGroundScreen)
                    continue;
                if (!enemy.isProjected)
                    computedSlots += std::max<std::int32_t>(1, enemy.directAttackSlots);
                if (enemy.replacementCost > 0)
                    ++m_liveGroundEnemyCount;
            }
            m_totalFrontDirectSlots = m_input.totalFrontDirectSlots > 0 ? m_input.totalFrontDirectSlots : computedSlots;

            m_enemyForwardCost.clear();
            m_enemyForwardCost.resize(m_input.enemies.size());
            for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
            {
                const auto & enemy = m_input.enemies[ei];
                if (enemy.hp <= 0.0 || enemy.tile == INVALID_TILE || enemy.movementPoints <= 0)
                    continue;
                const int eGrid = (enemy.gridId >= 0 && enemy.gridId < static_cast<int>(m_input.grids.size()))
                    ? enemy.gridId : (!m_input.grids.empty() ? 0 : -1);
                if (eGrid >= 0)
                {
                    pumpUiEventsIfDue();
                    m_enemyForwardCost[ei] = buildForwardDistances(eGrid, enemy.tile);
                }
            }

            // 1. Precompute forward Dijkstra from each site per unique grid so every enemy gets exact per-site tempo.
            std::vector<int> uniqueGrids;
            for (const auto & tmpl : m_input.templates)
            {
                if (tmpl.gridId >= 0 && tmpl.gridId < static_cast<int>(m_input.grids.size()) &&
                    std::ranges::find(uniqueGrids, tmpl.gridId) == uniqueGrids.end())
                {
                    uniqueGrids.push_back(tmpl.gridId);
                }
            }
            for (const int g : uniqueGrids)
            {
                auto & gridFwd = m_siteForwardCost[g];
                gridFwd.resize(siteCount);
                for (std::size_t si = 0; si < siteCount; ++si)
                {
                    pumpUiEventsIfDue();
                    gridFwd[si] = buildForwardDistances(g, m_input.sites[si].tile);
                }
            }

            std::map<std::tuple<int, std::int32_t, std::int32_t, std::vector<int>>, std::vector<std::int32_t>>
                enemyDistCache;
            for (std::size_t ti = 0; ti < tmplCount; ++ti)
            {
                pumpUiEventsIfDue();
                const auto & tmpl = m_input.templates[ti];
                if (!tmpl.hasWeapons || tmpl.movementPoints <= 0)
                    continue;
                std::vector<TilePoint> targetTiles;
                std::vector<int> targetIds;
                for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
                {
                    if (ei < tmpl.baseDamageVsEnemy.size() && tmpl.baseDamageVsEnemy[ei] > 0.0 &&
                        m_input.enemies[ei].tile != INVALID_TILE)
                    {
                        targetTiles.push_back(m_input.enemies[ei].tile);
                        targetIds.push_back(static_cast<int>(ei));
                    }
                }
                if (targetTiles.empty())
                    continue;
                const std::vector<std::int32_t> * distGrid = nullptr;
                if (tmpl.gridId >= 0 && tmpl.gridId < static_cast<int>(m_input.grids.size()))
                {
                    const auto key = std::tuple{tmpl.gridId, tmpl.minRange, tmpl.maxRange, targetIds};
                    auto [it, inserted] = enemyDistCache.emplace(key, std::vector<std::int32_t>{});
                    if (inserted)
                        it->second = buildReverseDistances(tmpl.gridId, targetTiles, tmpl.minRange, tmpl.maxRange);
                    distGrid = &it->second;
                }
                for (std::size_t si = 0; si < siteCount; ++si)
                {
                    const TilePoint st = m_input.sites[si].tile;
                    std::int32_t moveCost = std::numeric_limits<std::int32_t>::max();
                    if (distGrid != nullptr && st.x >= 0 && st.x < m_input.width && st.y >= 0 && st.y < m_input.height)
                    {
                        const std::size_t cell =
                            static_cast<std::size_t>(st.y) * static_cast<std::size_t>(m_input.width) +
                            static_cast<std::size_t>(st.x);
                        moveCost = (*distGrid)[cell];
                    }
                    if (moveCost < std::numeric_limits<std::int32_t>::max())
                    {
                        m_combatMoveTurns[si][ti] = (moveCost + tmpl.movementPoints - 1) / tmpl.movementPoints;
                    }
                }
            }

            // 2. Precompute capper distances from sites and between unowned properties per capper grid.
            std::vector<int> capperGrids;
            for (const auto & tmpl : m_input.templates)
            {
                if (tmpl.canCapture && tmpl.captureRate > 0 && tmpl.movementPoints > 0 &&
                    tmpl.gridId >= 0 && tmpl.gridId < static_cast<int>(m_input.grids.size()) &&
                    std::ranges::find(capperGrids, tmpl.gridId) == capperGrids.end())
                {
                    capperGrids.push_back(tmpl.gridId);
                }
            }
            for (const int g : capperGrids)
            {
                auto & siteToProp = m_siteToPropCost[g];
                siteToProp.assign(siteCount, std::vector<std::int32_t>(propCount, std::numeric_limits<std::int32_t>::max()));
                for (std::size_t si = 0; si < siteCount; ++si)
                {
                    const auto fwd = buildForwardDistances(g, m_input.sites[si].tile);
                    for (std::size_t pi = 0; pi < propCount; ++pi)
                    {
                        const TilePoint pt = m_input.properties[pi].tile;
                        if (pt.x >= 0 && pt.x < m_input.width && pt.y >= 0 && pt.y < m_input.height)
                        {
                            const std::size_t cell =
                                static_cast<std::size_t>(pt.y) * static_cast<std::size_t>(m_input.width) +
                                static_cast<std::size_t>(pt.x);
                            siteToProp[si][pi] = fwd[cell];
                        }
                    }
                }
                auto & propToProp = m_propToPropCost[g];
                propToProp.assign(propCount, std::vector<std::int32_t>(propCount, std::numeric_limits<std::int32_t>::max()));
                for (std::size_t pA = 0; pA < propCount; ++pA)
                {
                    const auto fwd = buildForwardDistances(g, m_input.properties[pA].tile);
                    for (std::size_t pB = 0; pB < propCount; ++pB)
                    {
                        const TilePoint pt = m_input.properties[pB].tile;
                        if (pt.x >= 0 && pt.x < m_input.width && pt.y >= 0 && pt.y < m_input.height)
                        {
                            const std::size_t cell =
                                static_cast<std::size_t>(pt.y) * static_cast<std::size_t>(m_input.width) +
                                static_cast<std::size_t>(pt.x);
                            propToProp[pA][pB] = fwd[cell];
                        }
                    }
                }
            }

            // Precompute each enemy's 1-turn threat and undiscounted remaining threat stream from each firing turn.
            const std::int32_t H = std::max<std::int32_t>(1, m_input.horizonTurns);
            m_enemyPerTurnThreat.assign(m_input.enemies.size(), 0);
            m_enemyThreatFromTurn.assign(
                m_input.enemies.size(),
                std::vector<MilliFunds>(static_cast<std::size_t>(std::max<std::int32_t>(2, H + 1)), 0));
            std::vector<int> ownBuildable;
            MilliFunds minOwnCost = 0;
            for (const auto & site : m_input.sites)
            {
                if (site.owner != OwnerSign::Ours)
                    continue;
                for (const int ti : site.buildableTemplateIndices)
                {
                    if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                        continue;
                    const MilliFunds tc = m_input.templates[static_cast<std::size_t>(ti)].cost;
                    if (tc > 0 && (minOwnCost == 0 || tc < minOwnCost))
                        minOwnCost = tc;
                    if (std::ranges::find(ownBuildable, ti) == ownBuildable.end())
                        ownBuildable.push_back(ti);
                }
            }
            const MilliFunds maxTargetCost = std::max(minOwnCost, m_input.totalFriendlyCombatCapital);
            MilliFunds minPropIncome = 0;
            for (const auto & prop : m_input.properties)
            {
                if (prop.income > 0 && (minPropIncome == 0 || prop.income < minPropIncome))
                    minPropIncome = prop.income;
            }
            for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
            {
                const auto & enemy = m_input.enemies[ei];
                MilliFunds best = std::max<MilliFunds>(0, enemy.perTurnThreat);
                if (best > 0)
                {
                    for (std::int32_t ft = 1; ft < H; ++ft)
                    {
                        m_enemyThreatFromTurn[ei][static_cast<std::size_t>(ft)] =
                            std::min(enemy.replacementCost, best * (H - ft));
                    }
                }
                else if (enemy.hp > 0.0)
                {
                    ProductionEnemyUnit fullEnemy = enemy;
                    fullEnemy.hp = UNIFIED_PROD_FULL_HP;
                    MilliFunds threatSum = 0;
                    std::int32_t threatCount = 0;
                    std::vector<double> stepThreatAvg(static_cast<std::size_t>(H), 0.0);
                    for (const int ti : ownBuildable)
                    {
                        const auto & tmpl = m_input.templates[static_cast<std::size_t>(ti)];
                        if (tmpl.cost <= 0 || tmpl.cost > maxTargetCost)
                            continue;
                        if (!tmpl.canMoveAndFire && m_input.friendlyIndirectCount <= 0)
                            continue;
                        const double eStrike = ei < tmpl.enemyFirstStrikeVsMe.size()
                            ? tmpl.enemyFirstStrikeVsMe[ei] : 0.0;
                        const double ourCounter = ei < tmpl.counterDamageWhenStruck.size()
                            ? tmpl.counterDamageWhenStruck[ei] : 0.0;
                        const MilliFunds threat = static_cast<MilliFunds>(std::llround(
                            enemyFirstStrikeNetThreat(tmpl, fullEnemy, eStrike, ourCounter)));
                        if (threat <= 0)
                            continue;
                        threatSum += threat;
                        ++threatCount;
                        double simEHp = UNIFIED_PROD_FULL_HP;
                        for (std::int32_t t = 1; t < H && simEHp > 0.0; ++t)
                        {
                            const double scaledS = eStrike * (simEHp / UNIFIED_PROD_FULL_HP);
                            const double dHp = std::min(UNIFIED_PROD_FULL_HP, scaledS / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                            const double uAfter = std::max(0.0, UNIFIED_PROD_FULL_HP - dHp);
                            const double uLoss = (static_cast<double>(tmpl.cost) * dHp) / UNIFIED_PROD_FULL_HP;
                            const double cHp = (enemy.canMoveAndFire && tmpl.canMoveAndFire && uAfter > 0.0 && ourCounter > 0.0)
                                ? std::min(simEHp, (ourCounter * (uAfter / UNIFIED_PROD_FULL_HP)) / UNIFIED_PROD_HP_PERCENT_PER_STEP)
                                : 0.0;
                            const double eLoss = (static_cast<double>(enemy.replacementCost) * cHp) / UNIFIED_PROD_FULL_HP;
                            const double netStep = uLoss - eLoss;
                            if (netStep <= 0.0)
                                break;
                            stepThreatAvg[static_cast<std::size_t>(t)] += netStep;
                            simEHp = std::max(0.0, simEHp - cHp);
                        }
                    }
                    if (threatCount > 0)
                    {
                        best = threatSum / threatCount;
                        double suffix = 0.0;
                        for (std::int32_t t = H - 1; t >= 1; --t)
                        {
                            suffix += stepThreatAvg[static_cast<std::size_t>(t)] / static_cast<double>(threatCount);
                            m_enemyThreatFromTurn[ei][static_cast<std::size_t>(t)] = std::min(
                                enemy.replacementCost, static_cast<MilliFunds>(std::llround(suffix)));
                        }
                    }
                }
                if (enemy.hp > 0.0 && enemy.canCapture && enemy.captureRate > 0 && minPropIncome > 0)
                {
                    const std::int32_t capTurns = std::max<std::int32_t>(
                        1, (m_input.capturePointsToCapture + enemy.captureRate - 1) / enemy.captureRate);
                    const MilliFunds capPerTurn = minPropIncome / capTurns;
                    best = std::max(best, capPerTurn);
                    for (std::int32_t ft = 1; ft < H; ++ft)
                    {
                        m_enemyThreatFromTurn[ei][static_cast<std::size_t>(ft)] = std::max(
                            m_enemyThreatFromTurn[ei][static_cast<std::size_t>(ft)],
                            std::min(enemy.replacementCost, capPerTurn * (H - ft)));
                    }
                }
                m_enemyPerTurnThreat[ei] = best;
            }

            // 3. Precompute per-turn marginal production surplus of capturing each unowned Factory, Airport, or Port.
            std::vector<int> ownSites;
            for (int i = 0; i < static_cast<int>(siteCount); ++i)
            {
                if (m_input.sites[static_cast<std::size_t>(i)].owner == OwnerSign::Ours)
                    ownSites.push_back(i);
            }
            const MilliFunds steadyBudget = std::max(m_input.currentFunds, m_input.nextTurnIncome);
            if (steadyBudget > 0)
            {
                const auto baseClaimed = buildClaimedTurns({});
                const auto baseCoverage = buildInitialCoverage();
                const std::int32_t baseBodies = effectiveScreenBodies();
                const std::int32_t baseDirectVehicles = effectiveDirectVehicles();
                const MilliFunds baseline = evaluateSingleTurnGreedy(
                    ownSites, steadyBudget, 0, baseCoverage, baseClaimed, m_input.friendlyDirectCapital,
                    baseDirectVehicles, m_input.friendlyCombatUnitCount, baseBodies, m_input.friendlyIndirectCount,
                    m_input.friendlyCapperCount).utility;
                for (int si = 0; si < static_cast<int>(siteCount); ++si)
                {
                    pumpUiEventsIfDue();
                    if (m_input.sites[static_cast<std::size_t>(si)].owner == OwnerSign::Ours)
                        continue;
                    std::vector<int> augmented = ownSites;
                    augmented.push_back(si);
                    const MilliFunds withSite = evaluateSingleTurnGreedy(
                        augmented, steadyBudget, 0, baseCoverage, baseClaimed, m_input.friendlyDirectCapital,
                        baseDirectVehicles, m_input.friendlyCombatUnitCount, baseBodies, m_input.friendlyIndirectCount,
                        m_input.friendlyCapperCount).utility;
                    if (withSite > baseline)
                        m_captureSitePerTurnSurplus.emplace(si, withSite - baseline);
                }
            }
        }

        int findSiteIndex(TilePoint tile, OwnerSign owner) const
        {
            for (int i = 0; i < static_cast<int>(m_input.sites.size()); ++i)
            {
                const auto & s = m_input.sites[static_cast<std::size_t>(i)];
                if (s.tile == tile && s.owner == owner)
                    return i;
            }
            return -1;
        }

        bool siteBuildsOnlyAir(int siteIdx) const
        {
            const auto & site = m_input.sites[static_cast<std::size_t>(siteIdx)];
            bool any = false;
            for (const int ti : site.buildableTemplateIndices)
            {
                if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                    continue;
                any = true;
                if (!m_input.templates[static_cast<std::size_t>(ti)].bypassesGroundDefense)
                    return false;
            }
            return any;
        }

        std::int32_t siteMinArrival(int siteIdx) const
        {
            std::int32_t best = UNIFIED_PROD_UNREACHABLE_TURNS;
            const auto & site = m_input.sites[static_cast<std::size_t>(siteIdx)];
            for (const int ti : site.buildableTemplateIndices)
            {
                if (ti >= 0 && ti < static_cast<int>(m_input.templates.size()))
                    best = std::min(best, m_combatMoveTurns[static_cast<std::size_t>(siteIdx)][static_cast<std::size_t>(ti)]);
            }
            return best;
        }

        std::vector<std::int32_t> buildClaimedTurns(std::span<const std::int32_t> overrideTurns) const
        {
            std::vector<std::int32_t> claimed(m_input.properties.size(), m_input.horizonTurns);
            for (std::size_t i = 0; i < m_input.properties.size(); ++i)
            {
                claimed[i] = (i < overrideTurns.size()) ? overrideTurns[i] : m_input.properties[i].claimedTurn;
            }
            return claimed;
        }

        std::vector<double> buildInitialCoverage() const
        {
            std::vector<double> cov(m_input.enemies.size(), 0.0);
            for (std::size_t i = 0; i < m_input.enemies.size(); ++i)
                cov[i] = std::max(0.0, m_input.enemies[i].initialFriendlyCoverage);
            return cov;
        }

        std::int32_t travelTurnsSiteToProp(int siteIdx, int propIdx, const ProductionUnitTemplate & tmpl) const
        {
            if (tmpl.movementPoints <= 0)
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            if (const auto it = m_siteToPropCost.find(tmpl.gridId); it != m_siteToPropCost.end())
            {
                const std::int32_t cost = it->second[static_cast<std::size_t>(siteIdx)][static_cast<std::size_t>(propIdx)];
                if (cost == std::numeric_limits<std::int32_t>::max())
                    return UNIFIED_PROD_UNREACHABLE_TURNS;
                return (cost + tmpl.movementPoints - 1) / tmpl.movementPoints;
            }
            return UNIFIED_PROD_UNREACHABLE_TURNS;
        }

        std::int32_t travelTurnsPropToProp(int pA, int pB, const ProductionUnitTemplate & tmpl) const
        {
            if (tmpl.movementPoints <= 0)
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            if (const auto it = m_propToPropCost.find(tmpl.gridId); it != m_propToPropCost.end())
            {
                const std::int32_t cost = it->second[static_cast<std::size_t>(pA)][static_cast<std::size_t>(pB)];
                if (cost == std::numeric_limits<std::int32_t>::max())
                    return UNIFIED_PROD_UNREACHABLE_TURNS;
                return (cost + tmpl.movementPoints - 1) / tmpl.movementPoints;
            }
            return UNIFIED_PROD_UNREACHABLE_TURNS;
        }

        MilliFunds propertyStreamValue(const ProductionCapturableProperty & prop, std::int32_t ownTurn) const
        {
            if (ownTurn >= m_input.horizonTurns)
                return 0;
            MilliFunds total = 0;
            if (prop.income > 0)
            {
                const std::int32_t ownedTurns = std::max<std::int32_t>(0, m_input.horizonTurns - ownTurn);
                const std::int32_t multiplier = prop.owner == OwnerSign::Enemy ? 2 : 1;
                const MilliFunds rawStream = static_cast<MilliFunds>(ownedTurns) * prop.income * multiplier;
                total += scalePaybackToRelevanceHorizon(rawStream, m_input.horizonTurns);
            }
            total += combatBonusCaptureValue(prop.tile, ownTurn, m_input.horizonTurns);
            total += siteCaptureProductionSurplus(prop.tile, ownTurn, m_input.horizonTurns);
            return total;
        }

        std::int32_t effectiveDirectVehicles() const
        {
            if (m_input.friendlyDirectVehicleCount > 0)
                return m_input.friendlyDirectVehicleCount;
            if (m_input.friendlyDirectCapital > 0)
                return std::max<std::int32_t>(1, m_input.friendlyCombatUnitCount - m_input.friendlyIndirectCount);
            return 0;
        }

        std::int32_t effectiveScreenBodies() const
        {
            if (m_input.friendlyScreenBodies > 0)
                return m_input.friendlyScreenBodies;
            if (m_input.friendlyCapperCount > 0 || m_input.friendlyDirectVehicleCount > 0)
                return 0;
            if (m_input.friendlyDirectCapital > 0)
            {
                if (m_input.enemyDirectCapital > 0)
                {
                    const MilliFunds unitRef = std::max<MilliFunds>(
                        1, m_input.enemyDirectCapital / std::max<std::int32_t>(1, effectiveEnemyDirectCount()));
                    return static_cast<std::int32_t>(
                        std::max<MilliFunds>(1, (m_input.friendlyDirectCapital + unitRef - 1) / unitRef));
                }
                return std::max<std::int32_t>(1, m_input.friendlyCombatUnitCount);
            }
            return 0;
        }

        std::int32_t enemyAttackTurnsToTile(std::size_t ei, TilePoint target) const
        {
            if (ei >= m_input.enemies.size() || target == INVALID_TILE)
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const auto & e = m_input.enemies[ei];
            if (e.tile == INVALID_TILE)
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const std::int32_t minR = std::max<std::int32_t>(1, e.minRange);
            const std::int32_t maxR = std::max<std::int32_t>(minR, e.maxRange);
            const int md = manhattanDistance(e.tile, target);
            if (md >= minR && md <= maxR)
                return e.spawnDelayTurns;
            if (!e.canMoveAndFire || e.movementPoints <= 0 ||
                ei >= m_enemyForwardCost.size() || m_enemyForwardCost[ei].empty())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const auto & fwd = m_enemyForwardCost[ei];
            const int x0 = std::max(0, target.x - maxR);
            const int x1 = std::min(m_input.width - 1, target.x + maxR);
            const int y0 = std::max(0, target.y - maxR);
            const int y1 = std::min(m_input.height - 1, target.y + maxR);
            std::int32_t bestCost = std::numeric_limits<std::int32_t>::max();
            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    const int d = manhattanDistance({x, y}, target);
                    if (d < minR || d > maxR)
                        continue;
                    const std::size_t idx =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(m_input.width) +
                        static_cast<std::size_t>(x);
                    if (idx < fwd.size() && fwd[idx] < bestCost)
                        bestCost = fwd[idx];
                }
            }
            if (bestCost == std::numeric_limits<std::int32_t>::max())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            return e.spawnDelayTurns + (bestCost + e.movementPoints - 1) / e.movementPoints;
        }

        TilePoint bestFiringSquareToEnemy(int siteIdx, bool fromSite, TilePoint fromTile,
                                          const ProductionUnitTemplate & tmpl, std::size_t enemyIdx) const
        {
            if (enemyIdx >= m_input.enemies.size())
                return INVALID_TILE;
            const TilePoint enemyTile = m_input.enemies[enemyIdx].tile;
            if (enemyTile == INVALID_TILE)
                return INVALID_TILE;
            const int effGrid = (tmpl.gridId >= 0 && tmpl.gridId < static_cast<int>(m_input.grids.size()))
                ? tmpl.gridId : (!m_input.grids.empty() ? 0 : -1);
            if (effGrid < 0)
                return INVALID_TILE;
            const std::vector<std::int32_t> * fwdPtr = nullptr;
            if (fromSite)
            {
                const auto it = m_siteForwardCost.find(effGrid);
                if (it != m_siteForwardCost.end() && siteIdx >= 0 &&
                    siteIdx < static_cast<int>(it->second.size()))
                    fwdPtr = &it->second[static_cast<std::size_t>(siteIdx)];
            }
            if (fwdPtr == nullptr && fromTile != INVALID_TILE)
            {
                const auto key = std::tuple{effGrid, fromTile.x, fromTile.y};
                auto it = m_tileForwardCost.find(key);
                if (it == m_tileForwardCost.end())
                    it = m_tileForwardCost.emplace(key, buildForwardDistances(effGrid, fromTile)).first;
                fwdPtr = &it->second;
            }
            if (fwdPtr == nullptr || fwdPtr->empty())
                return INVALID_TILE;
            const auto & fwd = *fwdPtr;
            const std::int32_t minR = std::max<std::int32_t>(1, tmpl.minRange);
            const std::int32_t maxR = std::max<std::int32_t>(minR, tmpl.maxRange);
            const int x0 = std::max(0, enemyTile.x - maxR);
            const int x1 = std::min(m_input.width - 1, enemyTile.x + maxR);
            const int y0 = std::max(0, enemyTile.y - maxR);
            const int y1 = std::min(m_input.height - 1, enemyTile.y + maxR);
            std::int32_t bestCost = std::numeric_limits<std::int32_t>::max();
            TilePoint bestTile = INVALID_TILE;
            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    const int d = manhattanDistance({x, y}, enemyTile);
                    if (d < minR || d > maxR)
                        continue;
                    const std::size_t idx =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(m_input.width) +
                        static_cast<std::size_t>(x);
                    if (idx < fwd.size() && fwd[idx] < bestCost)
                    {
                        bestCost = fwd[idx];
                        bestTile = {x, y};
                    }
                }
            }
            return bestTile;
        }

        std::int32_t siteToSiteAttackTurns(int fromSiteIdx, int toSiteIdx,
                                            const ProductionUnitTemplate & unitTmpl) const
        {
            if (unitTmpl.movementPoints <= 0 ||
                fromSiteIdx < 0 || fromSiteIdx >= static_cast<int>(m_input.sites.size()) ||
                toSiteIdx < 0 || toSiteIdx >= static_cast<int>(m_input.sites.size()))
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const TilePoint targetTile = m_input.sites[static_cast<std::size_t>(toSiteIdx)].tile;
            if (targetTile == INVALID_TILE)
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const auto it = m_siteForwardCost.find(unitTmpl.gridId);
            if (it == m_siteForwardCost.end() || fromSiteIdx >= static_cast<int>(it->second.size()))
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const auto & fwd = it->second[static_cast<std::size_t>(fromSiteIdx)];
            const std::int32_t minR = std::max<std::int32_t>(1, unitTmpl.minRange);
            const std::int32_t maxR = std::max<std::int32_t>(minR, unitTmpl.maxRange);
            const int x0 = std::max(0, targetTile.x - maxR);
            const int x1 = std::min(m_input.width - 1, targetTile.x + maxR);
            const int y0 = std::max(0, targetTile.y - maxR);
            const int y1 = std::min(m_input.height - 1, targetTile.y + maxR);
            std::int32_t bestCost = std::numeric_limits<std::int32_t>::max();
            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    const int d = manhattanDistance({x, y}, targetTile);
                    if (d < minR || d > maxR)
                        continue;
                    const std::size_t idx =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(m_input.width) +
                        static_cast<std::size_t>(x);
                    if (idx < fwd.size() && fwd[idx] < bestCost)
                        bestCost = fwd[idx];
                }
            }
            if (bestCost == std::numeric_limits<std::int32_t>::max())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const std::int32_t moveTurns = bestCost == 0 ? 0 : (bestCost + unitTmpl.movementPoints - 1) / unitTmpl.movementPoints;
            return moveTurns + ((!unitTmpl.canMoveAndFire && moveTurns > 0) ? 1 : 0);
        }

        std::int32_t effectiveEnemyDirectCount() const
        {
            if (m_input.enemyDirectCount > 0)
                return m_input.enemyDirectCount;
            std::int32_t count = 0;
            for (const auto & e : m_input.enemies)
            {
                if (e.hp > 0.0 && e.canMoveAndFire)
                    ++count;
            }
            return count;
        }

        std::int32_t moveTurnsSiteToEnemy(int siteIdx, const ProductionUnitTemplate & tmpl,
                                          std::size_t enemyIdx) const
        {
            if (tmpl.movementPoints <= 0 || enemyIdx >= m_input.enemies.size())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const TilePoint enemyTile = m_input.enemies[enemyIdx].tile;
            if (enemyTile == INVALID_TILE)
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const auto it = m_siteForwardCost.find(tmpl.gridId);
            if (it != m_siteForwardCost.end() && siteIdx >= 0 &&
                siteIdx < static_cast<int>(it->second.size()))
            {
                const auto & fwd = it->second[static_cast<std::size_t>(siteIdx)];
                const auto idxOf = [&](int x, int y) {
                    return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_input.width) +
                        static_cast<std::size_t>(x);
                };
                std::int32_t bestCost = std::numeric_limits<std::int32_t>::max();
                const int minR = std::max<std::int32_t>(1, tmpl.minRange);
                const int maxR = std::max<std::int32_t>(minR, tmpl.maxRange);
                const int x0 = std::max(0, enemyTile.x - maxR);
                const int x1 = std::min(m_input.width - 1, enemyTile.x + maxR);
                const int y0 = std::max(0, enemyTile.y - maxR);
                const int y1 = std::min(m_input.height - 1, enemyTile.y + maxR);
                for (int y = y0; y <= y1; ++y)
                {
                    for (int x = x0; x <= x1; ++x)
                    {
                        const int d = manhattanDistance({x, y}, enemyTile);
                        if (d < minR || d > maxR)
                            continue;
                        const std::int32_t c = fwd[idxOf(x, y)];
                        if (c < bestCost)
                            bestCost = c;
                    }
                }
                if (bestCost == std::numeric_limits<std::int32_t>::max())
                    return UNIFIED_PROD_UNREACHABLE_TURNS;
                return (bestCost + tmpl.movementPoints - 1) / tmpl.movementPoints;
            }
            return UNIFIED_PROD_UNREACHABLE_TURNS;
        }

        std::int32_t moveTurnsTileToEnemy(TilePoint origin, const ProductionUnitTemplate & tmpl,
                                          std::size_t enemyIdx) const
        {
            if (tmpl.movementPoints <= 0 || enemyIdx >= m_input.enemies.size())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            const TilePoint enemyTile = m_input.enemies[enemyIdx].tile;
            if (enemyTile == INVALID_TILE)
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            if (tmpl.gridId >= 0 && tmpl.gridId < static_cast<int>(m_input.grids.size()) &&
                origin.x >= 0 && origin.x < m_input.width && origin.y >= 0 && origin.y < m_input.height)
            {
                const std::int32_t minR = std::max<std::int32_t>(1, tmpl.minRange);
                const std::int32_t maxR = std::max<std::int32_t>(minR, tmpl.maxRange);
                const auto key = std::tuple{tmpl.gridId, minR, maxR, enemyIdx};
                auto it = m_enemyFireRingCost.find(key);
                if (it == m_enemyFireRingCost.end())
                {
                    it = m_enemyFireRingCost.emplace(
                        key, buildReverseDistances(tmpl.gridId, std::span<const TilePoint>(&enemyTile, 1), minR, maxR)).first;
                }
                const std::size_t cell =
                    static_cast<std::size_t>(origin.y) * static_cast<std::size_t>(m_input.width) +
                    static_cast<std::size_t>(origin.x);
                const std::int32_t cost = cell < it->second.size()
                    ? it->second[cell] : std::numeric_limits<std::int32_t>::max();
                if (cost == std::numeric_limits<std::int32_t>::max())
                    return UNIFIED_PROD_UNREACHABLE_TURNS;
                return (cost + tmpl.movementPoints - 1) / tmpl.movementPoints;
            }
            return UNIFIED_PROD_UNREACHABLE_TURNS;
        }

        // Traces one newly built capper sequentially over remaining horizon and optionally updates claimedTurns.
        MilliFunds evaluateCapperGain(int siteIdx, int tmplIdx, std::int32_t spawnTurn, bool hasCombatPresence,
                                      std::int32_t friendlyCapperCount,
                                      std::vector<std::int32_t> & claimedTurns, bool commitClaims) const
        {
            (void)friendlyCapperCount;
            const auto & tmpl = m_input.templates[static_cast<std::size_t>(tmplIdx)];
            if (!tmpl.canCapture || tmpl.captureRate <= 0 || tmpl.movementPoints <= 0 || m_input.properties.empty())
                return 0;
            const std::int32_t horizon = m_input.horizonTurns;
            const std::int32_t capTurns =
                (m_input.capturePointsToCapture + tmpl.captureRate - 1) / tmpl.captureRate;
            std::vector<std::int32_t> working = commitClaims ? std::vector<std::int32_t>{} : claimedTurns;
            auto & claims = commitClaims ? claimedTurns : working;

            std::int32_t curTurn = spawnTurn + 1;
            int curProp = -1;
            MilliFunds totalGain = 0;
            while (curTurn < horizon - 1)
            {
                int bestProp = -1;
                std::int32_t bestOwnTurn = horizon;
                MilliFunds bestGain = 0;
                for (int p = 0; p < static_cast<int>(m_input.properties.size()); ++p)
                {
                    const auto & prop = m_input.properties[static_cast<std::size_t>(p)];
                    if (prop.blocked || p == curProp ||
                        !(prop.owner == OwnerSign::Neutral || (hasCombatPresence && prop.owner == OwnerSign::Enemy)))
                        continue;
                    const std::int32_t moveTurns = curProp < 0
                        ? travelTurnsSiteToProp(siteIdx, p, tmpl)
                        : travelTurnsPropToProp(curProp, p, tmpl);
                    if (moveTurns >= UNIFIED_PROD_UNREACHABLE_TURNS)
                        continue;
                    const bool enemyDefendsProp = prop.owner == OwnerSign::Enemy && [&]() {
                        for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
                        {
                            const auto & e = m_input.enemies[ei];
                            if (e.hp <= 0.0 || e.isProjected || e.tile == INVALID_TILE)
                                continue;
                            if (e.tile == prop.tile)
                                return true;
                            if (!e.canCapture && e.replacementCost > 0 && enemyAttackTurnsToTile(ei, prop.tile) <= 1)
                                return true;
                        }
                        return false;
                    }();
                    const std::int32_t clearTurns = enemyDefendsProp ? 1 : 0;
                    const std::int32_t arrivalTurn = curTurn + std::max<std::int32_t>(1, moveTurns) - 1 + clearTurns;
                    const std::int32_t finishTurn = arrivalTurn + capTurns - 1;
                    const std::int32_t ownTurn = finishTurn + 1;
                    if (ownTurn >= claims[static_cast<std::size_t>(p)] || ownTurn >= horizon)
                        continue;
                    const MilliFunds afterVal = propertyStreamValue(prop, ownTurn);
                    const MilliFunds beforeVal = propertyStreamValue(prop, claims[static_cast<std::size_t>(p)]);
                    const MilliFunds gain = afterVal - beforeVal;
                    if (gain > bestGain || (gain == bestGain && gain > 0 && ownTurn < bestOwnTurn))
                    {
                        bestGain = gain;
                        bestProp = p;
                        bestOwnTurn = ownTurn;
                    }
                }
                if (bestProp < 0 || bestGain <= 0)
                    break;
                totalGain += bestGain;
                claims[static_cast<std::size_t>(bestProp)] = bestOwnTurn;
                curProp = bestProp;
                curTurn = bestOwnTurn;
            }
            return totalGain;
        }

        // Exact first-strike body absorption when screening uncovered friendly direct combat units.
        MilliFunds evaluateScreenGain(int siteIdx, int tmplIdx, std::int32_t spawnTurn,
                                      MilliFunds friendlyDirectCapital, std::int32_t friendlyCombatCount,
                                      std::int32_t friendlyScreenBodies) const
        {
            const auto & tmpl = m_input.templates[static_cast<std::size_t>(tmplIdx)];
            if (!tmpl.canMoveAndFire || tmpl.movementPoints <= 0 || tmpl.cost <= 0 ||
                friendlyCombatCount <= 0 || friendlyDirectCapital <= 0)
                return 0;
            const MilliFunds perCombatUnitCap = std::max<MilliFunds>(
                1, friendlyDirectCapital / std::max<std::int32_t>(1, friendlyCombatCount));
            if (tmpl.cost * 2 > friendlyDirectCapital)
                return 0;

            std::int32_t groundEnemyStrikers = 0;
            std::int32_t minMoveTurns = UNIFIED_PROD_UNREACHABLE_TURNS;
            MilliFunds bestShieldSurplus = 0;
            for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
            {
                const auto & enemy = m_input.enemies[ei];
                if (enemy.hp <= 0.0 || !enemy.canMoveAndFire || enemy.bypassesGroundScreen ||
                    (enemy.canCapture && enemy.replacementCost <= tmpl.cost) || enemy.spawnDelayTurns > 0)
                    continue;
                ++groundEnemyStrikers;
                const std::int32_t mt = moveTurnsSiteToEnemy(siteIdx, tmpl, ei);
                if (mt >= UNIFIED_PROD_UNREACHABLE_TURNS)
                    continue;
                minMoveTurns = std::min(minMoveTurns, mt);

                ProductionEnemyUnit fullEnemy = enemy;
                fullEnemy.hp = UNIFIED_PROD_FULL_HP;
                MilliFunds protectedThreat = ei < m_enemyPerTurnThreat.size() ? m_enemyPerTurnThreat[ei] : 0;
                for (const int pti : m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices)
                {
                    if (pti < 0 || pti >= static_cast<int>(m_input.templates.size()))
                        continue;
                    const auto & pTmpl = m_input.templates[static_cast<std::size_t>(pti)];
                    if (pTmpl.canCapture || !pTmpl.hasWeapons || pTmpl.cost > perCombatUnitCap)
                        continue;
                    if (!pTmpl.canMoveAndFire && m_input.friendlyIndirectCount <= 0)
                        continue;
                    const double eStrikeP = ei < pTmpl.enemyFirstStrikeVsMe.size()
                        ? pTmpl.enemyFirstStrikeVsMe[ei] : 0.0;
                    const double cDmgP = ei < pTmpl.counterDamageWhenStruck.size()
                        ? pTmpl.counterDamageWhenStruck[ei] : 0.0;
                    const double exchangeLoss = enemyFirstStrikeNetThreat(pTmpl, fullEnemy, eStrikeP, cDmgP);
                    // Losing first-strike initiative on a screened combat unit also forfeits its own clean attack swing.
                    const double pDealtHp = eStrikeP > 0.0
                        ? std::min(UNIFIED_PROD_FULL_HP, eStrikeP / UNIFIED_PROD_HP_PERCENT_PER_STEP)
                        : 0.0;
                    const double pAtk = ei < pTmpl.baseDamageVsEnemy.size() ? pTmpl.baseDamageVsEnemy[ei] : 0.0;
                    const double pRet = ei < pTmpl.returnDamageFromEnemy.size() ? pTmpl.returnDamageFromEnemy[ei] : 0.0;
                    const double lostFirstStrikeSwing = pDealtHp > 0.0
                        ? (pDealtHp / UNIFIED_PROD_FULL_HP) * firstStrikeNetTrade(pTmpl, fullEnemy, pAtk, pRet)
                        : 0.0;
                    const MilliFunds t = static_cast<MilliFunds>(std::llround(exchangeLoss + lostFirstStrikeSwing));
                    protectedThreat = std::max(protectedThreat, t);
                }
                const double eStrikeMe = ei < tmpl.enemyFirstStrikeVsMe.size() ? tmpl.enemyFirstStrikeVsMe[ei] : 0.0;
                const double scaledMe = eStrikeMe * (fullEnemy.hp / UNIFIED_PROD_FULL_HP);
                const double dealtMe = std::min(UNIFIED_PROD_FULL_HP, scaledMe / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                const MilliFunds screenBodyCost = static_cast<MilliFunds>(std::llround(
                    (static_cast<double>(tmpl.cost) * dealtMe) / UNIFIED_PROD_FULL_HP));
                if (protectedThreat > screenBodyCost && spawnTurn + 1 + mt < m_input.horizonTurns)
                    bestShieldSurplus = std::max(bestShieldSurplus, protectedThreat - screenBodyCost);
            }
            const std::int32_t neededScreens = std::min(groundEnemyStrikers, friendlyCombatCount);
            if (groundEnemyStrikers <= 0 || friendlyScreenBodies >= neededScreens ||
                spawnTurn + 1 + minMoveTurns >= m_input.horizonTurns)
                return 0;
            return bestShieldSurplus;
        }

        bool allLiveEnemiesCovered(std::span<const double> coverage) const
        {
            bool hasLive = false;
            for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
            {
                const auto & e = m_input.enemies[ei];
                if (e.hp <= 0.0 || e.replacementCost <= 0 || e.tile == INVALID_TILE)
                    continue;
                hasLive = true;
                const double cov = ei < coverage.size() ? coverage[ei] : 0.0;
                if (cov < 1.0)
                    return false;
            }
            return hasLive;
        }

        bool templateAntiAirCapable(int tmplIdx) const
        {
            if (tmplIdx < 0 || tmplIdx >= static_cast<int>(m_input.templates.size()))
                return false;
            const auto & tmpl = m_input.templates[static_cast<std::size_t>(tmplIdx)];
            if (!tmpl.hasWeapons || tmpl.cost <= 0)
                return false;
            for (std::size_t j = 0; j < m_input.templates.size() && j < tmpl.baseDamageVsTemplate.size(); ++j)
            {
                if (m_input.templates[j].bypassesGroundDefense && tmpl.baseDamageVsTemplate[j] > 0.0)
                    return true;
            }
            for (std::size_t ei = 0; ei < m_input.enemies.size() && ei < tmpl.baseDamageVsEnemy.size(); ++ei)
            {
                if (m_input.enemies[ei].bypassesGroundScreen && tmpl.baseDamageVsEnemy[ei] > 0.0)
                    return true;
            }
            return false;
        }

        // Sequential horizon combat trace (exact mirror of evaluateCapperGain): pure conservation, zero weights.
        [[gnu::noinline]] CombatEvalResult evaluateCombatGain(
            int siteIdx, int tmplIdx, std::int32_t spawnTurn, std::span<const double> coverage,
            MilliFunds friendlyDirectCapital, std::int32_t friendlyDirectVehicles,
            std::int32_t friendlyCombatCount, std::int32_t friendlyScreenBodies,
            std::int32_t friendlyIndirectCount, MilliFunds friendlyAntiAirCapital) const
        {
            (void)friendlyScreenBodies;
            (void)friendlyCombatCount;
            const auto & tmpl = m_input.templates[static_cast<std::size_t>(tmplIdx)];
            if (!tmpl.hasWeapons || tmpl.movementPoints <= 0 || m_input.enemies.empty())
                return {0, UNIFIED_PROD_UNREACHABLE_TURNS, {}};
            const std::int32_t fallbackMoveTurns =
                m_combatMoveTurns[static_cast<std::size_t>(siteIdx)][static_cast<std::size_t>(tmplIdx)];
            if (fallbackMoveTurns >= UNIFIED_PROD_UNREACHABLE_TURNS)
                return {0, UNIFIED_PROD_UNREACHABLE_TURNS, {}};

            const std::int32_t horizon = m_input.horizonTurns;
            const std::int32_t enemyCount = static_cast<std::int32_t>(m_input.enemies.size());
            MilliFunds liveDirectEnemyCap = m_input.enemyDirectCapital;
            if (liveDirectEnemyCap <= 0)
            {
                for (const auto & enemy : m_input.enemies)
                {
                    if (enemy.hp > 0.0 && enemy.canMoveAndFire && !enemy.bypassesGroundScreen &&
                        !enemy.canCapture && !enemy.isProjected)
                    {
                        liveDirectEnemyCap += static_cast<MilliFunds>(std::llround(
                            static_cast<double>(enemy.replacementCost) * (enemy.hp / UNIFIED_PROD_FULL_HP)));
                    }
                }
            }
            const MilliFunds directScreenAnchor = std::max(tmpl.cost * 2, liveDirectEnemyCap);
            const std::int32_t totalScreenBodies = friendlyDirectVehicles + friendlyScreenBodies;
            const bool indirectProtectedByScreen = !tmpl.canMoveAndFire && tmpl.maxRange > 1 &&
                friendlyDirectCapital > 0 &&
                friendlyDirectVehicles >= friendlyIndirectCount + 1 &&
                totalScreenBodies >= 2 * (friendlyIndirectCount + 1) &&
                friendlyDirectCapital >= static_cast<MilliFunds>(friendlyIndirectCount + 1) * directScreenAnchor;
            std::vector<std::uint8_t> engaged(static_cast<std::size_t>(enemyCount), 0);
            std::vector<std::pair<std::size_t, double>> claimedCoverage;

            std::int32_t curTurn = spawnTurn + 1;
            TilePoint curTile = m_input.sites[static_cast<std::size_t>(siteIdx)].tile;
            double curHp = UNIFIED_PROD_FULL_HP;
            std::int32_t remAmmo = tmpl.maxAmmo > 0 ? tmpl.maxAmmo : horizon;
            bool firstTarget = true;
            std::int32_t targetsEngaged = 0;
            std::int32_t firstFireTurn = UNIFIED_PROD_UNREACHABLE_TURNS;
            double totalOffense = 0.0;

            const bool reinforceMode = allLiveEnemiesCovered(coverage);
            while (curTurn < horizon && curHp > 0.0 && remAmmo > 0)
            {
                int bestEi = -1;
                double bestRate = 0.0;
                double bestGain = 0.0;
                double bestAfterHp = curHp;
                double bestClaimedFrac = 0.0;
                std::int32_t bestEndTurn = horizon;
                std::int32_t bestFireTurn = UNIFIED_PROD_UNREACHABLE_TURNS;
                std::int32_t bestAmmoSpent = 0;
                bool bestOpenFieldMobile = false;

                for (int ei = 0; ei < enemyCount; ++ei)
                {
                    if (engaged[static_cast<std::size_t>(ei)] != 0)
                        continue;
                    const auto & enemy = m_input.enemies[static_cast<std::size_t>(ei)];
                    if (enemy.hp <= 0.0 || enemy.replacementCost <= 0 || enemy.tile == INVALID_TILE)
                        continue;
                    const double cov = static_cast<std::size_t>(ei) < coverage.size()
                        ? coverage[static_cast<std::size_t>(ei)] : 0.0;
                    const double remaining = cov < 1.0
                        ? (1.0 - std::max(0.0, cov))
                        : ((reinforceMode && !enemy.bypassesGroundScreen) ? (1.0 / (1.0 + cov)) : 0.0);
                    if (remaining <= 0.0)
                        continue;
                    const double baseDmg = static_cast<std::size_t>(ei) < tmpl.baseDamageVsEnemy.size()
                        ? tmpl.baseDamageVsEnemy[static_cast<std::size_t>(ei)] : -1.0;
                    if (baseDmg <= 0.0)
                        continue;
                    const std::int32_t moveTurns = firstTarget
                        ? moveTurnsSiteToEnemy(siteIdx, tmpl, static_cast<std::size_t>(ei))
                        : moveTurnsTileToEnemy(curTile, tmpl, static_cast<std::size_t>(ei));
                    if (moveTurns >= UNIFIED_PROD_UNREACHABLE_TURNS)
                        continue;
                    // Indirects cannot move and fire on the same turn; narrow chokepoints delay extra melee congestion.
                    const std::int32_t setupTurn = (!tmpl.canMoveAndFire && (moveTurns > 0 || !firstTarget)) ? 1 : 0;
                    const std::int32_t frontSlots = std::max<std::int32_t>(enemy.directAttackSlots, m_totalFrontDirectSlots);
                    const bool congestedDirectChoke = tmpl.canMoveAndFire && tmpl.minRange == 1 &&
                        !tmpl.bypassesGroundDefense && !enemy.bypassesGroundScreen &&
                        frontSlots > 0 && friendlyDirectVehicles > frontSlots;
                    const std::int32_t indirectTargetCap = std::max<std::int32_t>(
                        1, std::min(totalScreenBodies / 2, m_liveGroundEnemyCount));
                    const bool congestedIndirect = !tmpl.canMoveAndFire && tmpl.maxRange > 1 &&
                        !tmpl.bypassesGroundDefense && friendlyIndirectCount >= indirectTargetCap;
                    const std::int32_t chokeQueueTurns = congestedDirectChoke
                        ? std::min<std::int32_t>(
                              2, (friendlyDirectVehicles - frontSlots) / std::max<std::int32_t>(1, frontSlots))
                        : (congestedIndirect
                               ? std::min<std::int32_t>(
                                     2, (friendlyIndirectCount - indirectTargetCap + 1) / indirectTargetCap)
                               : 0);
                    const std::int32_t earliestEngage = enemy.spawnDelayTurns + 1;
                    const std::int32_t fireTurn =
                        std::max(curTurn + moveTurns + setupTurn + chokeQueueTurns, earliestEngage + setupTurn);
                    if (fireTurn >= horizon)
                        continue;

                    const TilePoint bestSq = bestFiringSquareToEnemy(
                        siteIdx, firstTarget, curTile, tmpl, static_cast<std::size_t>(ei));
                    const TilePoint approachFrom = firstTarget
                        ? m_input.sites[static_cast<std::size_t>(siteIdx)].tile
                        : curTile;
                    const double retDmg = static_cast<std::size_t>(ei) < tmpl.returnDamageFromEnemy.size()
                        ? tmpl.returnDamageFromEnemy[static_cast<std::size_t>(ei)] : 0.0;
                    double simHp = curHp;
                    double simEnemyHp = enemy.hp * remaining;
                    std::int32_t simTurn = fireTurn;
                    std::int32_t simAmmo = remAmmo;
                    double targetGain = 0.0;
                    const bool openFieldMobile = !tmpl.canMoveAndFire && enemy.movementPoints > 0 &&
                        enemy.directAttackSlots > 1;
                    const auto & threatFromTurn = m_enemyThreatFromTurn[static_cast<std::size_t>(ei)];
                    // Unscreened indirects spending a setup turn absorb first strike before firing; ground screens block ground threats.
                    if (setupTurn > 0 && bestSq != INVALID_TILE)
                    {
                        double bestAmbushLoss = 0.0;
                        double bestAmbushHpLoss = 0.0;
                        for (int ej = 0; ej < enemyCount; ++ej)
                        {
                            const auto & amb = m_input.enemies[static_cast<std::size_t>(ej)];
                            if (indirectProtectedByScreen && !amb.bypassesGroundScreen)
                                continue;
                            const double ambCov = static_cast<std::size_t>(ej) < coverage.size()
                                ? coverage[static_cast<std::size_t>(ej)] : 0.0;
                            const double ambRem = std::max(0.0, 1.0 - std::max(0.0, ambCov));
                            if (!amb.canMoveAndFire || ambRem <= 0.0 || amb.hp <= 0.0)
                                continue;
                            if (enemyAttackTurnsToTile(static_cast<std::size_t>(ej), bestSq) > fireTurn - spawnTurn)
                                continue;
                            const double ambStrike = static_cast<std::size_t>(ej) < tmpl.enemyFirstStrikeVsMe.size()
                                ? tmpl.enemyFirstStrikeVsMe[static_cast<std::size_t>(ej)] : 0.0;
                            if (ambStrike <= 0.0)
                                continue;
                            const double ambHp = amb.hp * ambRem;
                            const double dealtToUs = std::min(
                                simHp, (ambStrike * (ambHp / UNIFIED_PROD_FULL_HP)) / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                            const double loss = (static_cast<double>(tmpl.cost) * dealtToUs) / UNIFIED_PROD_FULL_HP;
                            if (loss > bestAmbushLoss)
                            {
                                bestAmbushLoss = loss;
                                bestAmbushHpLoss = dealtToUs;
                            }
                        }
                        simHp = std::max(0.0, simHp - bestAmbushHpLoss);
                        targetGain -= bestAmbushLoss;
                    }
                    else if (tmpl.canMoveAndFire && moveTurns > 1 && enemy.directAttackSlots > 1 &&
                             enemy.canMoveAndFire &&
                             enemyAttackTurnsToTile(static_cast<std::size_t>(ei), approachFrom) < moveTurns)
                    {
                        const double eStrike = static_cast<std::size_t>(ei) < tmpl.enemyFirstStrikeVsMe.size()
                            ? tmpl.enemyFirstStrikeVsMe[static_cast<std::size_t>(ei)] : 0.0;
                        if (eStrike > 0.0 && simEnemyHp > 0.0)
                        {
                            const double dealtToUs = std::min(
                                simHp, (eStrike * (simEnemyHp / UNIFIED_PROD_FULL_HP)) / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                            const double afterUs = std::max(0.0, simHp - dealtToUs);
                            const double usLoss = (static_cast<double>(tmpl.cost) * dealtToUs) / UNIFIED_PROD_FULL_HP;
                            double cHp = 0.0;
                            double cCapGain = 0.0;
                            double cDenial = 0.0;
                            if (tmpl.minRange <= 1 && enemy.minRange <= 1 && afterUs > 0.0)
                            {
                                const double cPct = static_cast<std::size_t>(ei) < tmpl.counterDamageWhenStruck.size()
                                    ? tmpl.counterDamageWhenStruck[static_cast<std::size_t>(ei)] : 0.0;
                                cHp = cPct > 0.0
                                    ? std::min(simEnemyHp,
                                               (cPct * (afterUs / UNIFIED_PROD_FULL_HP)) / UNIFIED_PROD_HP_PERCENT_PER_STEP)
                                    : 0.0;
                                cCapGain = (static_cast<double>(enemy.replacementCost) * cHp) / UNIFIED_PROD_FULL_HP;
                                const double cRemStream =
                                    (cov < 1.0 && simTurn > 0 && static_cast<std::size_t>(simTurn) < threatFromTurn.size())
                                    ? static_cast<double>(threatFromTurn[static_cast<std::size_t>(simTurn)]) * remaining
                                    : 0.0;
                                cDenial = cRemStream * (cHp / UNIFIED_PROD_FULL_HP);
                            }
                            simHp = afterUs;
                            simEnemyHp = std::max(0.0, simEnemyHp - cHp);
                            targetGain += (cCapGain + cDenial - usLoss);
                        }
                    }
                    while (simTurn < horizon && simHp > 0.0 && simEnemyHp > 0.0 && simAmmo > 0)
                    {
                        if (openFieldMobile && simTurn > fireTurn)
                            break;
                        const double scaledDmg = baseDmg * (simHp / UNIFIED_PROD_FULL_HP);
                        const double dealtHp = std::min(simEnemyHp, scaledDmg / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                        if (dealtHp <= 0.0)
                            break;
                        const double afterEnemyHp = std::max(0.0, simEnemyHp - dealtHp);
                        const double enemyCapLoss =
                            (static_cast<double>(enemy.replacementCost) * dealtHp) / UNIFIED_PROD_FULL_HP;
                        const double remThreatStream =
                            (cov < 1.0 && simTurn > 0 && static_cast<std::size_t>(simTurn) < threatFromTurn.size())
                            ? static_cast<double>(threatFromTurn[static_cast<std::size_t>(simTurn)]) * remaining
                            : 0.0;
                        const double denialSaved = remThreatStream * (dealtHp / UNIFIED_PROD_FULL_HP);
                        double ourCounterLoss = 0.0;
                        double retHp = 0.0;
                        if (tmpl.canMoveAndFire && afterEnemyHp > 0.0 && retDmg > 0.0)
                        {
                            const double scaledRet = retDmg * (afterEnemyHp / UNIFIED_PROD_FULL_HP);
                            retHp = std::min(simHp, scaledRet / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                            ourCounterLoss = (static_cast<double>(tmpl.cost) * retHp) / UNIFIED_PROD_FULL_HP;
                        }
                        else if (!tmpl.canMoveAndFire && afterEnemyHp > 0.0 && enemy.canMoveAndFire &&
                                 !(indirectProtectedByScreen && !enemy.bypassesGroundScreen) &&
                                 bestSq != INVALID_TILE &&
                                 (enemyAttackTurnsToTile(static_cast<std::size_t>(ei), bestSq) - enemy.spawnDelayTurns) <= 1)
                        {
                            const double eStrike = static_cast<std::size_t>(ei) < tmpl.enemyFirstStrikeVsMe.size()
                                ? tmpl.enemyFirstStrikeVsMe[static_cast<std::size_t>(ei)] : 0.0;
                            if (eStrike > 0.0)
                            {
                                const double scaledStrike = eStrike * (afterEnemyHp / UNIFIED_PROD_FULL_HP);
                                retHp = std::min(simHp, scaledStrike / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                                ourCounterLoss = (static_cast<double>(tmpl.cost) * retHp) / UNIFIED_PROD_FULL_HP;
                            }
                        }
                        const double stepNet = enemyCapLoss + denialSaved - ourCounterLoss;
                        if (stepNet <= 0.0 && (setupTurn == 0 || simTurn > fireTurn))
                            break;
                        targetGain += stepNet;
                        simEnemyHp = afterEnemyHp;
                        simHp = std::max(0.0, simHp - retHp);
                        ++simTurn;
                        --simAmmo;
                    }
                    if (targetGain <= 0.0)
                        continue;
                    const std::int32_t turnsUsed = std::max<std::int32_t>(1, simTurn - curTurn);
                    const double rate = targetGain / static_cast<double>(turnsUsed);
                    if (rate > bestRate || (rate == bestRate && targetGain > bestGain))
                    {
                        bestRate = rate;
                        bestGain = targetGain;
                        bestAfterHp = simHp;
                        const double startEnemyHp = enemy.hp * remaining;
                        bestClaimedFrac = enemy.hp > 0.0
                            ? std::max(0.0, std::min(remaining, (startEnemyHp - simEnemyHp) / enemy.hp))
                            : 0.0;
                        bestEndTurn = simTurn;
                        bestFireTurn = fireTurn;
                        bestAmmoSpent = remAmmo - simAmmo;
                        bestOpenFieldMobile = openFieldMobile;
                        bestEi = ei;
                    }
                }
                if (bestEi < 0 || bestGain <= 0.0)
                    break;
                if (firstTarget)
                    firstFireTurn = bestFireTurn;
                firstTarget = false;
                ++targetsEngaged;
                engaged[static_cast<std::size_t>(bestEi)] = 1;
                if (bestClaimedFrac > 0.0)
                    claimedCoverage.emplace_back(static_cast<std::size_t>(bestEi), bestClaimedFrac);
                totalOffense += bestGain;
                curTurn = bestEndTurn;
                curHp = bestAfterHp;
                remAmmo -= bestAmmoSpent;
                curTile = m_input.enemies[static_cast<std::size_t>(bestEi)].tile;
                if (bestOpenFieldMobile && !tmpl.canMoveAndFire && !indirectProtectedByScreen)
                    break;
            }

            if (firstFireTurn >= UNIFIED_PROD_UNREACHABLE_TURNS)
                firstFireTurn = spawnTurn + 1 + fallbackMoveTurns + (tmpl.canMoveAndFire ? 0 : 1);
            if (totalOffense <= 0.0)
                return {0, firstFireTurn, {}};

            // Counter-exposure against reachable in-horizon enemies and connected enemy production sites.
            const auto siteFwdIt = m_siteForwardCost.find(tmpl.gridId);
            const auto siteToTileTurns = [&](TilePoint dest) -> std::int32_t {
                if (siteFwdIt == m_siteForwardCost.end() || siteIdx < 0 ||
                    siteIdx >= static_cast<int>(siteFwdIt->second.size()) ||
                    dest.x < 0 || dest.x >= m_input.width || dest.y < 0 || dest.y >= m_input.height)
                    return UNIFIED_PROD_UNREACHABLE_TURNS;
                const auto & fwd = siteFwdIt->second[static_cast<std::size_t>(siteIdx)];
                const std::size_t cell = static_cast<std::size_t>(dest.y) * static_cast<std::size_t>(m_input.width) +
                    static_cast<std::size_t>(dest.x);
                if (cell >= fwd.size() || fwd[cell] == std::numeric_limits<std::int32_t>::max())
                    return UNIFIED_PROD_UNREACHABLE_TURNS;
                return (fwd[cell] + std::max<std::int32_t>(1, tmpl.movementPoints) - 1) /
                    std::max<std::int32_t>(1, tmpl.movementPoints);
            };
            double maxCounterRisk = 0.0;
            for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
            {
                const auto & enemy = m_input.enemies[ei];
                if (indirectProtectedByScreen && !enemy.bypassesGroundScreen)
                    continue;
                if (enemy.hp <= 0.0 || enemy.replacementCost <= 0)
                    continue;
                const bool hasEnemyPos = enemy.tile != INVALID_TILE;
                const std::int32_t eReachTurns = hasEnemyPos ? moveTurnsSiteToEnemy(siteIdx, tmpl, ei) : 1;
                if (hasEnemyPos && (eReachTurns >= UNIFIED_PROD_UNREACHABLE_TURNS || spawnTurn + 1 + eReachTurns >= horizon))
                    continue;
                const double baseDmg = ei < tmpl.baseDamageVsEnemy.size() ? tmpl.baseDamageVsEnemy[ei] : -1.0;
                const double retDmg = ei < tmpl.returnDamageFromEnemy.size() ? tmpl.returnDamageFromEnemy[ei] : 0.0;
                const double enemyStrike = ei < tmpl.enemyFirstStrikeVsMe.size() ? tmpl.enemyFirstStrikeVsMe[ei] : 0.0;
                const double ourCounter = ei < tmpl.counterDamageWhenStruck.size() ? tmpl.counterDamageWhenStruck[ei] : 0.0;
                const std::int32_t enemyTurnsToUs = hasEnemyPos
                    ? enemyAttackTurnsToTile(ei, m_input.sites[static_cast<std::size_t>(siteIdx)].tile)
                    : UNIFIED_PROD_UNREACHABLE_TURNS;
                if (hasEnemyPos && tmpl.canMoveAndFire && enemy.canMoveAndFire && enemyTurnsToUs > eReachTurns)
                    continue;
                const double cov = ei < coverage.size() ? coverage[ei] : 0.0;
                const double remaining = cov < 1.0
                    ? (1.0 - std::max(0.0, cov))
                    : ((reinforceMode && !enemy.bypassesGroundScreen) ? (1.0 / (1.0 + cov)) : 0.0);
                double counterThreat = 0.0;
                if (remaining > 0.0)
                {
                    const double netTrade = firstStrikeNetTrade(tmpl, enemy, baseDmg, retDmg, tmpl.minRange);
                    ProductionEnemyUnit remEnemy = enemy;
                    remEnemy.hp = enemy.hp * remaining;
                    const double threat = enemyFirstStrikeNetThreat(tmpl, remEnemy, enemyStrike, ourCounter, enemy.minRange);
                    if ((hasEnemyPos && enemyTurnsToUs < eReachTurns) || !enemy.canMoveAndFire)
                        counterThreat = threat;
                    else
                        counterThreat = std::max(0.0, threat - netTrade);
                }
                maxCounterRisk = std::max(maxCounterRisk, counterThreat);
            }
            for (int eSiteIdx = 0; eSiteIdx < static_cast<int>(m_input.sites.size()); ++eSiteIdx)
            {
                const auto & eSite = m_input.sites[static_cast<std::size_t>(eSiteIdx)];
                if (eSite.owner != OwnerSign::Enemy)
                    continue;
                const std::int32_t sReachTurns = siteToTileTurns(eSite.tile);
                if (sReachTurns >= UNIFIED_PROD_UNREACHABLE_TURNS || spawnTurn + 1 + sReachTurns >= horizon)
                    continue;
                const MilliFunds siteBudget =
                    eSite.ownerAvailableFunds > 0 ? eSite.ownerAvailableFunds : m_input.nextTurnIncome;
                const std::int32_t ourCorridorTurns = siteToSiteAttackTurns(siteIdx, eSiteIdx, tmpl);
                for (const int cti : eSite.buildableTemplateIndices)
                {
                    if (cti < 0 || cti >= static_cast<int>(m_input.templates.size()))
                        continue;
                    const auto & ct = m_input.templates[static_cast<std::size_t>(cti)];
                    if (!ct.hasWeapons || !ct.canMoveAndFire || ct.canCapture || ct.cost <= 0)
                        continue;
                    if (indirectProtectedByScreen && !ct.bypassesGroundDefense)
                        continue;
                    if (!tmpl.bypassesGroundDefense && ct.bypassesGroundDefense &&
                        (m_input.enemyAirCapital <= 0 ||
                         friendlyAntiAirCapital >= m_input.enemyAirCapital + ct.cost / 2))
                        continue;
                    if (siteBudget > 0 && ct.cost > siteBudget)
                        continue;
                    const std::int32_t ctCorridorTurns = siteToSiteAttackTurns(eSiteIdx, siteIdx, ct);
                    if (ctCorridorTurns >= UNIFIED_PROD_UNREACHABLE_TURNS)
                        continue;
                    if (tmpl.canMoveAndFire && ctCorridorTurns > ourCorridorTurns)
                        continue;
                    if (static_cast<std::size_t>(tmplIdx) >= ct.baseDamageVsTemplate.size() ||
                        static_cast<std::size_t>(cti) >= tmpl.baseDamageVsTemplate.size())
                        continue;
                    const double ctStrike = ct.baseDamageVsTemplate[static_cast<std::size_t>(tmplIdx)];
                    if (ctStrike <= 0.0)
                        continue;
                    const double myDmg = tmpl.baseDamageVsTemplate[static_cast<std::size_t>(cti)];
                    const double ctRet = (tmpl.canMoveAndFire && ct.canMoveAndFire) ? std::max(0.0, ctStrike) : 0.0;
                    const double myCounter =
                        (ct.canMoveAndFire && tmpl.canMoveAndFire && tmpl.hasWeapons) ? std::max(0.0, myDmg) : 0.0;
                    ProductionEnemyUnit ctEnemy;
                    ctEnemy.replacementCost = ct.cost;
                    ctEnemy.hp = UNIFIED_PROD_FULL_HP;
                    ctEnemy.movementPoints = ct.movementPoints;
                    ctEnemy.minRange = ct.minRange;
                    ctEnemy.maxRange = ct.maxRange;
                    ctEnemy.canMoveAndFire = ct.canMoveAndFire;
                    ctEnemy.bypassesGroundScreen = ct.bypassesGroundDefense;
                    const double rawExcess = std::max(
                        0.0, enemyFirstStrikeNetThreat(tmpl, ctEnemy, ctStrike, myCounter, ct.minRange) -
                                 firstStrikeNetTrade(tmpl, ctEnemy, myDmg, ctRet, tmpl.minRange));
                    const double parityShare = std::min(1.0, static_cast<double>(tmpl.cost) / static_cast<double>(ct.cost));
                    maxCounterRisk = std::max(maxCounterRisk, rawExcess * parityShare);
                }
            }

            const double net = std::max(0.0, totalOffense - maxCounterRisk);
            return {static_cast<MilliFunds>(std::llround(net)), firstFireTurn, std::move(claimedCoverage)};
        }

        void applyClaimedCoverage(int tmplIdx, std::span<const std::pair<std::size_t, double>> claimed,
                                  std::vector<double> & coverage) const
        {
            if (claimed.empty())
            {
                applyCoverageUpdate(tmplIdx, coverage);
                return;
            }
            const MilliFunds capBudget = tmplIdx >= 0 && tmplIdx < static_cast<int>(m_input.templates.size())
                ? std::max<MilliFunds>(0, m_input.templates[static_cast<std::size_t>(tmplIdx)].cost)
                : 0;
            const bool reinforceMode = allLiveEnemiesCovered(coverage);
            MilliFunds claimedCap = 0;
            for (std::size_t idx = 0; idx < claimed.size(); ++idx)
            {
                const auto [ei, frac] = claimed[idx];
                if (ei >= coverage.size() || ei >= m_input.enemies.size() || frac <= 0.0)
                    continue;
                const auto & enemy = m_input.enemies[ei];
                const double liveCap = (static_cast<double>(enemy.replacementCost) * enemy.hp) / UNIFIED_PROD_FULL_HP;
                if (liveCap <= 0.0)
                    continue;
                double effFrac = frac;
                if (idx > 0 && capBudget > 0)
                {
                    const MilliFunds remCapBudget = std::max<MilliFunds>(0, capBudget - claimedCap);
                    if (remCapBudget <= 0)
                        break;
                    effFrac = std::min(effFrac, static_cast<double>(remCapBudget) / liveCap);
                }
                if (effFrac <= 0.0)
                    continue;
                coverage[ei] = reinforceMode ? (coverage[ei] + effFrac) : std::min(1.0, coverage[ei] + effFrac);
                claimedCap += static_cast<MilliFunds>(std::llround(effFrac * liveCap));
            }
        }

        void applyCoverageUpdate(int tmplIdx, std::vector<double> & coverage) const
        {
            const auto & tmpl = m_input.templates[static_cast<std::size_t>(tmplIdx)];
            if (!tmpl.hasWeapons)
                return;
            const bool reinforceMode = allLiveEnemiesCovered(coverage);
            // Prioritize air and highest-value uncovered threats so anti-air covers copters before infantry.
            struct CovCand
            {
                std::size_t ei{0};
                double share{0.0};
                int isAir{0};
                MilliFunds cost{0};
            };
            std::vector<CovCand> cands;
            for (std::size_t ei = 0; ei < m_input.enemies.size() && ei < coverage.size(); ++ei)
            {
                const auto & enemy = m_input.enemies[ei];
                if (enemy.hp <= 0.0 || enemy.replacementCost <= 0)
                    continue;
                if (reinforceMode && enemy.bypassesGroundScreen)
                    continue;
                const double rem = reinforceMode
                    ? 1.0
                    : std::max(0.0, 1.0 - std::max(0.0, coverage[ei]));
                if (rem <= 0.0)
                    continue;
                const double liveCap = (static_cast<double>(enemy.replacementCost) * enemy.hp) / UNIFIED_PROD_FULL_HP;
                const double baseDmg = ei < tmpl.baseDamageVsEnemy.size() ? tmpl.baseDamageVsEnemy[ei] : -1.0;
                const double retDmg = ei < tmpl.returnDamageFromEnemy.size() ? tmpl.returnDamageFromEnemy[ei] : 0.0;
                const double netTrade = firstStrikeNetTrade(tmpl, enemy, baseDmg, retDmg);
                if (netTrade > 0.0 && liveCap > 0.0)
                {
                    const double share = std::min(rem, netTrade / liveCap);
                    if (share > 0.0)
                        cands.push_back({ei, share, enemy.bypassesGroundScreen ? 1 : 0, enemy.replacementCost});
                }
            }
            if (cands.empty())
                return;
            std::stable_sort(cands.begin(), cands.end(), [](const CovCand & a, const CovCand & b) {
                return std::tuple{a.isAir, a.cost, a.share} > std::tuple{b.isAir, b.cost, b.share};
            });
            double budgetLeft = 1.0;
            for (const auto & c : cands)
            {
                if (budgetLeft <= 0.0)
                    break;
                const double alloc = std::min(budgetLeft, c.share);
                coverage[c.ei] = reinforceMode
                    ? (coverage[c.ei] + alloc)
                    : std::min(1.0, coverage[c.ei] + alloc);
                budgetLeft -= alloc;
            }
        }

        CandidateScore scoreSiteTemplate(int siteIdx, int tmplIdx, std::int32_t spawnTurn,
                                         std::span<const double> coverage,
                                         std::vector<std::int32_t> & claimedTurns,
                                         MilliFunds friendlyDirectCapital,
                                         std::int32_t friendlyDirectVehicles,
                                         std::int32_t friendlyCombatCount,
                                         std::int32_t friendlyScreenBodies,
                                         std::int32_t friendlyIndirectCount,
                                         std::int32_t friendlyCapperCount, bool commitClaims,
                                         MilliFunds friendlyAntiAirCapital = -1) const
        {
            const MilliFunds effAntiAirCap =
                friendlyAntiAirCapital >= 0 ? friendlyAntiAirCapital : m_input.friendlyAntiAirCapital;
            const auto & tmpl = m_input.templates[static_cast<std::size_t>(tmplIdx)];
            CandidateScore out;
            out.templateIndex = tmplIdx;
            out.cost = tmpl.cost;
            if (tmpl.cost <= 0)
                return out;
            const bool hasCombatPresence =
                friendlyDirectCapital > 0 || friendlyCombatCount > 0 || m_input.totalFriendlyCombatCapital > 0;
            out.capperGain = evaluateCapperGain(
                siteIdx, tmplIdx, spawnTurn, hasCombatPresence, friendlyCapperCount, claimedTurns, commitClaims);
            CombatEvalResult combatRes = evaluateCombatGain(
                siteIdx, tmplIdx, spawnTurn, coverage, friendlyDirectCapital, friendlyDirectVehicles,
                friendlyCombatCount, friendlyScreenBodies, friendlyIndirectCount, effAntiAirCap);
            out.combatGain = combatRes.gain;
            out.arrivalTurns = combatRes.firstFireTurn;
            out.claimedCoverage = std::move(combatRes.claimedCoverage);
            out.screenGain = evaluateScreenGain(
                siteIdx, tmplIdx, spawnTurn, friendlyDirectCapital, friendlyCombatCount, friendlyScreenBodies);
            // Conserve single-unit action economy: each unit performs one primary role (capture, combat, or screen).
            out.utility = std::max({out.capperGain, out.combatGain, out.screenGain});

            const auto & site = m_input.sites[static_cast<std::size_t>(siteIdx)];
            if (spawnTurn == 0 && site.owner == OwnerSign::Ours &&
                (!site.enemyCanStrikeInE0.empty() || !site.enemyCapperCanStepInE0.empty()))
            {
                std::int32_t ownSiteCount = 0;
                for (const auto & s : m_input.sites)
                    if (s.owner == OwnerSign::Ours) ++ownSiteCount;
                const MilliFunds incomeHold = ownSiteCount > 0 ? m_input.nextTurnIncome / ownSiteCount : 0;
                const MilliFunds strategicHold = m_input.horizonTurns > 1
                    ? ownStrategicCaptureLoss(site.tile, m_input.horizonTurns) /
                          std::max<std::int32_t>(1, m_input.horizonTurns - 1)
                    : 0;
                const MilliFunds oneTurnSiteHoldValue = std::max(incomeHold, strategicHold);
                const double defFactor = tmpl.bypassesGroundDefense
                    ? 1.0
                    : std::max(UNIFIED_PROD_MIN_DEFENSE_FACTOR,
                               (UNIFIED_PROD_PERCENT_SCALE -
                                static_cast<double>(site.defenseStars) * UNIFIED_PROD_DEFENSE_PER_STAR_PCT) /
                                   UNIFIED_PROD_PERCENT_SCALE);

                std::vector<std::size_t> strikers;
                for (std::size_t ei = 0; ei < site.enemyCanStrikeInE0.size() && ei < m_input.enemies.size(); ++ei)
                {
                    if (site.enemyCanStrikeInE0[ei] != 0 && m_input.enemies[ei].hp > 0.0)
                        strikers.push_back(ei);
                }
                std::stable_sort(strikers.begin(), strikers.end(), [&](std::size_t a, std::size_t b) {
                    const double dA = a < tmpl.enemyFirstStrikeVsMe.size()
                        ? tmpl.enemyFirstStrikeVsMe[a] * m_input.enemies[a].hp : 0.0;
                    const double dB = b < tmpl.enemyFirstStrikeVsMe.size()
                        ? tmpl.enemyFirstStrikeVsMe[b] * m_input.enemies[b].hp : 0.0;
                    return dA > dB;
                });

                double remHp = UNIFIED_PROD_FULL_HP;
                MilliFunds ourSpawnLoss = 0;
                MilliFunds enemyCounterLoss = 0;
                std::vector<std::uint8_t> usedKillers(m_input.enemies.size(), 0);
                for (const std::size_t ei : strikers)
                {
                    if (remHp <= 0.0)
                        break;
                    const auto & enemy = m_input.enemies[ei];
                    const double rawStrike = ei < tmpl.enemyFirstStrikeVsMe.size() ? tmpl.enemyFirstStrikeVsMe[ei] : 0.0;
                    const double scaledStrike = rawStrike * (enemy.hp / UNIFIED_PROD_FULL_HP) * defFactor;
                    const double hitHp = std::min(remHp, scaledStrike / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                    if (hitHp <= 0.0)
                        continue;
                    const double afterUs = std::max(0.0, remHp - hitHp);
                    const double rawC = (afterUs > 0.0 && enemy.canMoveAndFire && tmpl.canMoveAndFire && tmpl.hasWeapons &&
                                         ei < tmpl.counterDamageWhenStruck.size())
                        ? tmpl.counterDamageWhenStruck[ei] : 0.0;
                    const double counterHp = std::min(
                        enemy.hp, (rawC * (afterUs / UNIFIED_PROD_FULL_HP)) / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                    const MilliFunds usLoss = static_cast<MilliFunds>(std::llround(
                        (static_cast<double>(tmpl.cost) * hitHp) / UNIFIED_PROD_FULL_HP));
                    const MilliFunds eLoss = static_cast<MilliFunds>(std::llround(
                        (static_cast<double>(enemy.replacementCost) * counterHp) / UNIFIED_PROD_FULL_HP));
                    if (usLoss > eLoss || afterUs <= 0.0)
                    {
                        remHp = afterUs;
                        ourSpawnLoss += usLoss;
                        enemyCounterLoss += eLoss;
                        usedKillers[ei] = 1;
                    }
                }
                bool anyCapperReaches = false;
                bool unspentCapperFree = false;
                for (std::size_t ei = 0; ei < site.enemyCapperCanStepInE0.size() && ei < m_input.enemies.size(); ++ei)
                {
                    if (site.enemyCapperCanStepInE0[ei] == 0)
                        continue;
                    anyCapperReaches = true;
                    if (usedKillers[ei] == 0)
                        unspentCapperFree = true;
                }
                if (remHp <= 0.0)
                {
                    const MilliFunds holdCredit = (anyCapperReaches && !unspentCapperFree) ? oneTurnSiteHoldValue : 0;
                    out.capperGain = 0;
                    out.combatGain = 0;
                    out.utility = std::max<MilliFunds>(0, holdCredit + enemyCounterLoss - tmpl.cost);
                }
                else
                {
                    const double frac = remHp / UNIFIED_PROD_FULL_HP;
                    out.capperGain = static_cast<MilliFunds>(std::llround(static_cast<double>(out.capperGain) * frac));
                    out.combatGain = static_cast<MilliFunds>(std::llround(static_cast<double>(out.combatGain) * frac));
                    out.screenGain = static_cast<MilliFunds>(std::llround(static_cast<double>(out.screenGain) * frac));
                    const MilliFunds bestRole = std::max({out.capperGain, out.combatGain, out.screenGain});
                    const MilliFunds holdCredit = anyCapperReaches ? oneTurnSiteHoldValue : 0;
                    const MilliFunds idleCost = (!tmpl.hasWeapons && !tmpl.canCapture && bestRole <= 0)
                        ? tmpl.cost
                        : ourSpawnLoss;
                    out.utility = std::max<MilliFunds>(0, bestRole + holdCredit + enemyCounterLoss - idleCost);
                }
            }
            return out;
        }

        std::vector<int> paretoTemplatesForSite(int siteIdx, MilliFunds budget, std::int32_t spawnTurn,
                                                std::span<const double> coverage,
                                                std::vector<std::int32_t> claimedTurns,
                                                MilliFunds friendlyDirectCapital,
                                                std::int32_t friendlyDirectVehicles,
                                                std::int32_t friendlyCombatCount,
                                                std::int32_t friendlyScreenBodies,
                                                std::int32_t friendlyIndirectCount,
                                                std::int32_t friendlyCapperCount,
                                                MilliFunds friendlyAntiAirCapital) const
        {
            const auto & site = m_input.sites[static_cast<std::size_t>(siteIdx)];
            std::vector<CandidateScore> capperScored;
            std::vector<CandidateScore> combatScored;
            for (const int ti : site.buildableTemplateIndices)
            {
                if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                    continue;
                if (m_input.templates[static_cast<std::size_t>(ti)].cost > budget)
                    continue;
                const auto sc = scoreSiteTemplate(
                    siteIdx, ti, spawnTurn, coverage, claimedTurns, friendlyDirectCapital,
                    friendlyDirectVehicles, friendlyCombatCount, friendlyScreenBodies,
                    friendlyIndirectCount, friendlyCapperCount, false, friendlyAntiAirCapital);
                if (sc.utility <= 0)
                    continue;
                if (m_input.templates[static_cast<std::size_t>(ti)].canCapture)
                    capperScored.push_back(sc);
                else
                    combatScored.push_back(sc);
            }
            const auto paretoOf = [](std::vector<CandidateScore> & list) {
                std::ranges::sort(list, [](const CandidateScore & a, const CandidateScore & b) {
                    return std::tuple{a.cost, -a.utility, a.templateIndex} <
                        std::tuple{b.cost, -b.utility, b.templateIndex};
                });
                std::vector<CandidateScore> out;
                MilliFunds bestU = 0;
                for (const auto & sc : list)
                {
                    if (sc.utility > bestU)
                    {
                        out.push_back(sc);
                        bestU = sc.utility;
                    }
                }
                return out;
            };
            std::vector<CandidateScore> capPareto = paretoOf(capperScored);
            std::vector<CandidateScore> cmbPareto = paretoOf(combatScored);
            std::vector<CandidateScore> combined;
            combined.reserve(capPareto.size() + cmbPareto.size());
            // Run Pareto separately so a high-capper Infantry cannot prune Tanks/Armor off the combat frontier.
            for (const auto & sc : cmbPareto)
                combined.push_back(sc);
            for (const auto & sc : capPareto)
                combined.push_back(sc);
            if (combined.empty())
                return {};
            if (combined.size() > UNIFIED_PROD_PARETO_PER_SITE)
            {
                std::vector<CandidateScore> capped;
                capped.reserve(UNIFIED_PROD_PARETO_PER_SITE);
                std::vector<std::uint8_t> used(combined.size(), 0);
                if (!capPareto.empty())
                {
                    auto bestCap = std::ranges::max_element(capPareto, [](const auto & a, const auto & b) {
                        return a.utility < b.utility;
                    });
                    for (std::size_t i = 0; i < combined.size(); ++i)
                    {
                        if (combined[i].templateIndex == bestCap->templateIndex)
                        {
                            capped.push_back(combined[i]);
                            used[i] = 1;
                            break;
                        }
                    }
                }
                std::vector<std::size_t> rest;
                for (std::size_t i = 0; i < combined.size(); ++i)
                    if (!used[i]) rest.push_back(i);
                std::ranges::sort(rest, [&](std::size_t a, std::size_t b) {
                    return combined[a].utility > combined[b].utility;
                });
                for (const std::size_t idx : rest)
                {
                    if (capped.size() >= UNIFIED_PROD_PARETO_PER_SITE)
                        break;
                    capped.push_back(combined[idx]);
                }
                combined = std::move(capped);
            }
            std::vector<int> ids;
            ids.reserve(combined.size());
            for (const auto & sc : combined)
                ids.push_back(sc.templateIndex);
            return ids;
        }

        struct GreedyTurnOutcome
        {
            MilliFunds utility{0};
            MilliFunds remainingFunds{0};
            std::vector<double> coverage;
            std::vector<std::int32_t> claims;
            MilliFunds directCapital{0};
            std::int32_t directVehicles{0};
            std::int32_t combatCount{0};
            std::int32_t screenBodies{0};
            std::int32_t indirectCount{0};
            std::int32_t capperCount{0};
            MilliFunds antiAirCapital{0};
        };

        [[gnu::noinline]] GreedyTurnOutcome evaluateSingleTurnGreedy(std::span<const int> siteIndices, MilliFunds budget,
                                                   std::int32_t spawnTurn, std::vector<double> coverage,
                                                   std::vector<std::int32_t> claimedTurns,
                                                   MilliFunds friendlyDirectCapital,
                                                   std::int32_t friendlyDirectVehicles,
                                                   std::int32_t friendlyCombatCount,
                                                   std::int32_t friendlyScreenBodies,
                                                   std::int32_t friendlyIndirectCount,
                                                   std::int32_t friendlyCapperCount,
                                                   MilliFunds friendlyAntiAirCapital = -1) const
        {
            const MilliFunds initAntiAir =
                friendlyAntiAirCapital >= 0 ? friendlyAntiAirCapital : m_input.friendlyAntiAirCapital;
            GreedyTurnOutcome emptyOut{0, std::max<MilliFunds>(0, budget), coverage, claimedTurns,
                                       friendlyDirectCapital, friendlyDirectVehicles, friendlyCombatCount,
                                       friendlyScreenBodies, friendlyIndirectCount, friendlyCapperCount, initAntiAir};
            if (siteIndices.empty() || budget <= 0)
                return emptyOut;
            // Compare greedy efficiency pack and raw-utility pack, plus single highest-value unit + greedy fill.
            const auto runGreedy = [&](int forcedSite, int forcedTmpl, bool packByRawUtility) -> GreedyTurnOutcome {
                GreedyTurnOutcome out;
                out.remainingFunds = budget;
                out.coverage = coverage;
                out.claims = claimedTurns;
                out.directCapital = friendlyDirectCapital;
                out.directVehicles = friendlyDirectVehicles;
                out.combatCount = friendlyCombatCount;
                out.screenBodies = friendlyScreenBodies;
                out.indirectCount = friendlyIndirectCount;
                out.capperCount = friendlyCapperCount;
                out.antiAirCapital = initAntiAir;
                std::vector<std::uint8_t> used(siteIndices.size(), 0);
                const auto advanceState = [&](int ti, const CandidateScore & sc) {
                    if (sc.combatGain >= sc.capperGain)
                        applyClaimedCoverage(ti, sc.claimedCoverage, out.coverage);
                    const auto & ft = m_input.templates[static_cast<std::size_t>(ti)];
                    if (ft.hasWeapons)
                    {
                        ++out.combatCount;
                        if (templateAntiAirCapable(ti))
                            out.antiAirCapital += sc.cost;
                        if (ft.canMoveAndFire && !ft.canCapture)
                        {
                            out.directCapital += sc.cost;
                            ++out.directVehicles;
                        }
                        else if (!ft.canMoveAndFire && ft.maxRange > 1)
                        {
                            ++out.indirectCount;
                        }
                    }
                    if (ft.canCapture)
                    {
                        ++out.capperCount;
                        if (ft.canMoveAndFire && sc.capperGain < std::max(sc.combatGain, sc.screenGain))
                            ++out.screenBodies;
                    }
                };
                if (forcedSite >= 0 && forcedTmpl >= 0)
                {
                    const auto sc = scoreSiteTemplate(
                        siteIndices[static_cast<std::size_t>(forcedSite)], forcedTmpl, spawnTurn, out.coverage,
                        out.claims, out.directCapital, out.directVehicles, out.combatCount, out.screenBodies,
                        out.indirectCount, out.capperCount, true, out.antiAirCapital);
                    if (sc.cost > out.remainingFunds || sc.utility <= 0)
                        return emptyOut;
                    out.remainingFunds -= sc.cost;
                    out.utility += sc.utility;
                    used[static_cast<std::size_t>(forcedSite)] = 1;
                    advanceState(forcedTmpl, sc);
                }
                for (;;)
                {
                    int bestS = -1;
                    CandidateScore bestCand;
                    double bestEfficiency = -1.0;
                    for (int s = 0; s < static_cast<int>(siteIndices.size()); ++s)
                    {
                        if (used[static_cast<std::size_t>(s)])
                            continue;
                        const int siteIdx = siteIndices[static_cast<std::size_t>(s)];
                        for (const int ti : m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices)
                        {
                            if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                                continue;
                            if (m_input.templates[static_cast<std::size_t>(ti)].cost > out.remainingFunds)
                                continue;
                            const auto sc = scoreSiteTemplate(
                                siteIdx, ti, spawnTurn, out.coverage, out.claims, out.directCapital,
                                out.directVehicles, out.combatCount, out.screenBodies, out.indirectCount,
                                out.capperCount, false, out.antiAirCapital);
                            if (sc.utility <= 0)
                                continue;
                            const double eff = packByRawUtility
                                ? static_cast<double>(sc.utility)
                                : (static_cast<double>(sc.utility) /
                                   static_cast<double>(std::max<MilliFunds>(1, sc.cost)));
                            if (eff > bestEfficiency ||
                                (eff == bestEfficiency && sc.utility > bestCand.utility))
                            {
                                bestEfficiency = eff;
                                bestCand = sc;
                                bestS = s;
                            }
                        }
                    }
                    if (bestS < 0 || bestCand.utility <= 0)
                        break;
                    const auto committed = scoreSiteTemplate(
                        siteIndices[static_cast<std::size_t>(bestS)], bestCand.templateIndex, spawnTurn, out.coverage,
                        out.claims, out.directCapital, out.directVehicles, out.combatCount, out.screenBodies,
                        out.indirectCount, out.capperCount, true, out.antiAirCapital);
                    out.remainingFunds -= committed.cost;
                    out.utility += committed.utility;
                    used[static_cast<std::size_t>(bestS)] = 1;
                    advanceState(committed.templateIndex, committed);
                }
                return out;
            };

            GreedyTurnOutcome best = runGreedy(-1, -1, false);
            GreedyTurnOutcome rawPack = runGreedy(-1, -1, true);
            if (rawPack.utility > best.utility)
                best = std::move(rawPack);
            int topSite = -1;
            int topTmpl = -1;
            MilliFunds topRawUtility = 0;
            for (int s = 0; s < static_cast<int>(siteIndices.size()); ++s)
            {
                const int siteIdx = siteIndices[static_cast<std::size_t>(s)];
                for (const int ti : m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices)
                {
                    if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                        continue;
                    if (m_input.templates[static_cast<std::size_t>(ti)].cost > budget)
                        continue;
                    const auto sc = scoreSiteTemplate(
                        siteIdx, ti, spawnTurn, coverage, claimedTurns, friendlyDirectCapital,
                        friendlyDirectVehicles, friendlyCombatCount, friendlyScreenBodies,
                        friendlyIndirectCount, friendlyCapperCount, false, initAntiAir);
                    if (sc.utility > topRawUtility)
                    {
                        topRawUtility = sc.utility;
                        topSite = s;
                        topTmpl = ti;
                    }
                }
            }
            if (topSite >= 0)
            {
                GreedyTurnOutcome alt = runGreedy(topSite, topTmpl, false);
                if (alt.utility > best.utility)
                    best = std::move(alt);
                GreedyTurnOutcome altRaw = runGreedy(topSite, topTmpl, true);
                if (altRaw.utility > best.utility)
                    best = std::move(altRaw);
            }
            return best;
        }

        MilliFunds evaluateTomorrowBest(std::span<const int> allOwnSites, MilliFunds tomorrowFunds,
                                        const std::vector<double> & coverage,
                                        const std::vector<std::int32_t> & claimedTurns,
                                        MilliFunds friendlyDirectCapital,
                                        std::int32_t friendlyDirectVehicles,
                                        std::int32_t friendlyCombatCount,
                                        std::int32_t friendlyScreenBodies,
                                        std::int32_t friendlyIndirectCount,
                                        std::int32_t friendlyCapperCount,
                                        MilliFunds friendlyAntiAirCapital = -1) const
        {
            if (allOwnSites.empty() || tomorrowFunds <= 0 || m_input.horizonTurns <= 1)
                return 0;
            // Roll multi-turn continuation across waves 1..UNIFIED_PROD_FUTURE_WAVES so carried cash retains true future value.
            MilliFunds totalFuture = 0;
            MilliFunds curRem = tomorrowFunds;
            std::vector<double> curCov = coverage;
            std::vector<std::int32_t> curClaims = claimedTurns;
            MilliFunds curDirect = friendlyDirectCapital;
            std::int32_t curDirectVehicles = friendlyDirectVehicles;
            std::int32_t curCombat = friendlyCombatCount;
            std::int32_t curScreen = friendlyScreenBodies;
            std::int32_t curIndirect = friendlyIndirectCount;
            std::int32_t curCapper = friendlyCapperCount;
            MilliFunds curAntiAir = friendlyAntiAirCapital >= 0 ? friendlyAntiAirCapital : m_input.friendlyAntiAirCapital;
            for (std::int32_t wave = 1; wave <= UNIFIED_PROD_FUTURE_WAVES && wave < m_input.horizonTurns; ++wave)
            {
                const GreedyTurnOutcome out = evaluateSingleTurnGreedy(
                    allOwnSites, curRem, wave, curCov, curClaims,
                    curDirect, curDirectVehicles, curCombat, curScreen, curIndirect, curCapper, curAntiAir);
                totalFuture += out.utility;
                curRem = out.remainingFunds + m_input.nextTurnIncome;
                curCov = std::move(out.coverage);
                curClaims = std::move(out.claims);
                curDirect = out.directCapital;
                curDirectVehicles = out.directVehicles;
                curCombat = out.combatCount;
                curScreen = out.screenBodies;
                curIndirect = out.indirectCount;
                curCapper = out.capperCount;
                curAntiAir = out.antiAirCapital;
            }
            return totalFuture;
        }

        void searchExactToday(std::span<const int> openSiteIndices, std::span<const int> allOwnSiteIndices,
                              MilliFunds spendableFunds, const std::vector<double> & baseCoverage,
                              const std::vector<std::int32_t> & baseClaimed, ProductionPlan & bestPlan) const
        {
            const std::int32_t baseBodies = effectiveScreenBodies();
            const std::int32_t baseCombatCount = m_input.friendlyCombatUnitCount;
            const std::int32_t baseDirectVehicles = effectiveDirectVehicles();

            std::vector<ProductionBuildOrder> currentOrders;
            currentOrders.reserve(openSiteIndices.size());
            const auto dfs = [&](auto & self, std::size_t depth, MilliFunds remFunds, MilliFunds todayVal,
                                 std::vector<double> & cov, std::vector<std::int32_t> & claims,
                                 MilliFunds directCap, std::int32_t directVehicles, std::int32_t combatCount,
                                 std::int32_t screenBodies, std::int32_t indirectCount,
                                 std::int32_t capperCount, MilliFunds antiAirCap) -> void {
                pumpUiEventsIfDue();
                if (depth == openSiteIndices.size())
                {
                    const MilliFunds tomorrowVal = evaluateTomorrowBest(
                        allOwnSiteIndices,
                        std::max<MilliFunds>(0, remFunds + m_input.nextTurnIncome - m_input.nextTurnRepairBill),
                        cov, claims, directCap, directVehicles, combatCount, screenBodies,
                        indirectCount, capperCount, antiAirCap);
                    const MilliFunds totalVal = todayVal + tomorrowVal;
                    const bool better = totalVal > bestPlan.totalValue ||
                        (totalVal == bestPlan.totalValue && todayVal > bestPlan.todayUtility);
                    if (better)
                    {
                        bestPlan.orders = currentOrders;
                        bestPlan.todayUtility = todayVal;
                        bestPlan.tomorrowUtility = tomorrowVal;
                        bestPlan.totalValue = totalVal;
                        bestPlan.spentFunds = spendableFunds - remFunds;
                        bestPlan.remainingFunds = remFunds;
                    }
                    return;
                }
                const int siteIdx = openSiteIndices[depth];
                // Recompute Pareto per branch so Site 1 building frontline armor unlocks Infantry/Indirects at Site 2.
                std::vector<int> options = paretoTemplatesForSite(
                    siteIdx, remFunds, 0, cov, claims, directCap, directVehicles, combatCount,
                    screenBodies, indirectCount, capperCount, antiAirCap);
                options.push_back(-1);
                for (const int ti : options)
                {
                    if (ti < 0)
                    {
                        self(self, depth + 1, remFunds, todayVal, cov, claims, directCap, directVehicles,
                             combatCount, screenBodies, indirectCount, capperCount, antiAirCap);
                        continue;
                    }
                    const auto & tmpl = m_input.templates[static_cast<std::size_t>(ti)];
                    if (tmpl.cost > remFunds)
                        continue;
                    std::vector<double> nextCov = cov;
                    std::vector<std::int32_t> nextClaims = claims;
                    const auto sc = scoreSiteTemplate(
                        siteIdx, ti, 0, nextCov, nextClaims, directCap, directVehicles, combatCount,
                        screenBodies, indirectCount, capperCount, true, antiAirCap);
                    if (sc.utility <= 0)
                        continue;
                    if (sc.combatGain >= sc.capperGain)
                        applyClaimedCoverage(ti, sc.claimedCoverage, nextCov);
                    const bool isCombatUnit = tmpl.hasWeapons;
                    const bool isDirectVehicle = isCombatUnit && tmpl.canMoveAndFire && !tmpl.canCapture;
                    const MilliFunds nextDirect = directCap + (isDirectVehicle ? tmpl.cost : 0);
                    const std::int32_t nextDirectVehicles = directVehicles + (isDirectVehicle ? 1 : 0);
                    const std::int32_t nextCombatCount = combatCount + (isCombatUnit ? 1 : 0);
                    const std::int32_t nextBodies = screenBodies +
                        ((tmpl.canCapture && tmpl.canMoveAndFire && sc.capperGain < std::max(sc.combatGain, sc.screenGain))
                             ? 1 : 0);
                    const std::int32_t nextIndirectCount =
                        indirectCount + ((isCombatUnit && !tmpl.canMoveAndFire && tmpl.maxRange > 1) ? 1 : 0);
                    const std::int32_t nextCapperCount = capperCount + (tmpl.canCapture ? 1 : 0);
                    const MilliFunds nextAntiAir = antiAirCap + (templateAntiAirCapable(ti) ? sc.cost : 0);
                    currentOrders.push_back(ProductionBuildOrder{
                        .tile = m_input.sites[static_cast<std::size_t>(siteIdx)].tile,
                        .unitId = tmpl.unitId,
                        .templateIndex = ti,
                        .cost = sc.cost,
                        .utility = sc.utility,
                        .capperGain = sc.capperGain,
                        .combatGain = sc.combatGain,
                        .arrivalTurns = sc.arrivalTurns,
                    });
                    self(self, depth + 1, remFunds - sc.cost, todayVal + sc.utility, nextCov, nextClaims,
                         nextDirect, nextDirectVehicles, nextCombatCount, nextBodies, nextIndirectCount,
                         nextCapperCount, nextAntiAir);
                    currentOrders.pop_back();
                }
            };
            std::vector<double> cov = baseCoverage;
            std::vector<std::int32_t> claims = baseClaimed;
            dfs(dfs, 0, spendableFunds, 0, cov, claims, m_input.friendlyDirectCapital,
                baseDirectVehicles, baseCombatCount, baseBodies, m_input.friendlyIndirectCount,
                m_input.friendlyCapperCount, m_input.friendlyAntiAirCapital);
        }

        [[gnu::noinline]] bool evaluateOrderAssignment(std::span<const int> openSiteIndices, std::span<const int> chosenTmplPerPos,
                                     std::span<const int> allOwnSiteIndices, MilliFunds spendableFunds,
                                     const std::vector<double> & baseCoverage,
                                     const std::vector<std::int32_t> & baseClaimed,
                                     ProductionPlan & outPlan) const
        {
            if (chosenTmplPerPos.size() != openSiteIndices.size())
                return false;
            MilliFunds rem = spendableFunds;
            MilliFunds todayVal = 0;
            std::vector<double> cov = baseCoverage;
            std::vector<std::int32_t> claims = baseClaimed;
            MilliFunds directCap = m_input.friendlyDirectCapital;
            std::int32_t directVehicles = effectiveDirectVehicles();
            std::int32_t combatCount = m_input.friendlyCombatUnitCount;
            std::int32_t screenBodies = effectiveScreenBodies();
            std::int32_t indirectCount = m_input.friendlyIndirectCount;
            std::int32_t capperCount = m_input.friendlyCapperCount;
            MilliFunds antiAirCap = m_input.friendlyAntiAirCapital;
            std::vector<ProductionBuildOrder> orders;
            orders.reserve(openSiteIndices.size());

            for (std::size_t pos = 0; pos < openSiteIndices.size(); ++pos)
            {
                const int ti = chosenTmplPerPos[pos];
                if (ti < 0)
                    continue;
                if (ti >= static_cast<int>(m_input.templates.size()))
                    return false;
                const int siteIdx = openSiteIndices[pos];
                const auto & tmpl = m_input.templates[static_cast<std::size_t>(ti)];
                if (tmpl.cost <= 0 || tmpl.cost > rem)
                    return false;
                const auto sc = scoreSiteTemplate(
                    siteIdx, ti, 0, cov, claims, directCap, directVehicles, combatCount, screenBodies,
                    indirectCount, capperCount, true, antiAirCap);
                if (sc.utility <= 0)
                    return false;
                if (sc.combatGain >= sc.capperGain)
                    applyClaimedCoverage(ti, sc.claimedCoverage, cov);
                const bool isCombatUnit = tmpl.hasWeapons;
                const bool isDirectVehicle = isCombatUnit && tmpl.canMoveAndFire && !tmpl.canCapture;
                if (isDirectVehicle)
                {
                    directCap += sc.cost;
                    ++directVehicles;
                }
                if (isCombatUnit)
                {
                    ++combatCount;
                    if (templateAntiAirCapable(ti))
                        antiAirCap += sc.cost;
                }
                if (tmpl.canCapture)
                {
                    ++capperCount;
                    if (tmpl.canMoveAndFire && sc.capperGain < std::max(sc.combatGain, sc.screenGain))
                        ++screenBodies;
                }
                if (isCombatUnit && !tmpl.canMoveAndFire && tmpl.maxRange > 1)
                    ++indirectCount;
                rem -= sc.cost;
                todayVal += sc.utility;
                orders.push_back(ProductionBuildOrder{
                    .tile = m_input.sites[static_cast<std::size_t>(siteIdx)].tile,
                    .unitId = tmpl.unitId,
                    .templateIndex = ti,
                    .cost = sc.cost,
                    .utility = sc.utility,
                    .capperGain = sc.capperGain,
                    .combatGain = sc.combatGain,
                    .arrivalTurns = sc.arrivalTurns,
                });
            }
            const MilliFunds tomorrowVal = evaluateTomorrowBest(
                allOwnSiteIndices,
                std::max<MilliFunds>(0, rem + m_input.nextTurnIncome - m_input.nextTurnRepairBill),
                cov, claims, directCap, directVehicles, combatCount, screenBodies, indirectCount, capperCount,
                antiAirCap);
            outPlan.orders = std::move(orders);
            outPlan.todayUtility = todayVal;
            outPlan.tomorrowUtility = tomorrowVal;
            outPlan.totalValue = todayVal + tomorrowVal;
            outPlan.spentFunds = spendableFunds - rem;
            outPlan.remainingFunds = rem;
            return true;
        }

        [[gnu::noinline]] void searchGreedyWithBigAlternatives(std::span<const int> openSiteIndices, std::span<const int> allOwnSiteIndices,
                                             MilliFunds spendableFunds, const std::vector<double> & baseCoverage,
                                             const std::vector<std::int32_t> & baseClaimed,
                                             ProductionPlan & bestPlan) const
        {
            const std::int32_t baseBodies = effectiveScreenBodies();
            const std::int32_t baseCombatCount = m_input.friendlyCombatUnitCount;
            const std::int32_t baseDirectVehicles = effectiveDirectVehicles();
            const auto evalCandidate = [&](int forcedPos, int forcedTmpl, bool packByRawUtility) {
                pumpUiEventsIfDue();
                MilliFunds rem = spendableFunds;
                MilliFunds todayVal = 0;
                std::vector<double> cov = baseCoverage;
                std::vector<std::int32_t> claims = baseClaimed;
                MilliFunds directCap = m_input.friendlyDirectCapital;
                std::int32_t directVehicles = baseDirectVehicles;
                std::int32_t combatCount = baseCombatCount;
                std::int32_t screenBodies = baseBodies;
                std::int32_t indirectCount = m_input.friendlyIndirectCount;
                std::int32_t capperCount = m_input.friendlyCapperCount;
                MilliFunds antiAirCap = m_input.friendlyAntiAirCapital;
                std::vector<std::uint8_t> used(openSiteIndices.size(), 0);
                std::vector<ProductionBuildOrder> orders;

                auto tryAdd = [&](int pos, int ti) -> bool {
                    const int siteIdx = openSiteIndices[static_cast<std::size_t>(pos)];
                    const auto & tmpl = m_input.templates[static_cast<std::size_t>(ti)];
                    if (tmpl.cost > rem)
                        return false;
                    std::vector<std::int32_t> trialClaims = claims;
                    const auto sc = scoreSiteTemplate(
                        siteIdx, ti, 0, cov, trialClaims, directCap, directVehicles, combatCount, screenBodies,
                        indirectCount, capperCount, true, antiAirCap);
                    if (sc.utility <= 0)
                        return false;
                    // Only build if taking it today plus reduced tomorrow cash beats stopping here.
                    std::vector<double> nextCov = cov;
                    if (sc.combatGain >= sc.capperGain)
                        applyClaimedCoverage(ti, sc.claimedCoverage, nextCov);
                    const bool isCombatUnit = tmpl.hasWeapons;
                    const bool isDirectVehicle = isCombatUnit && tmpl.canMoveAndFire && !tmpl.canCapture;
                    const MilliFunds nextDirect = directCap + (isDirectVehicle ? tmpl.cost : 0);
                    const std::int32_t nextDirectVehicles = directVehicles + (isDirectVehicle ? 1 : 0);
                    const std::int32_t nextCombatCount = combatCount + (isCombatUnit ? 1 : 0);
                    const std::int32_t nextBodies = screenBodies +
                        ((tmpl.canCapture && tmpl.canMoveAndFire && sc.capperGain < std::max(sc.combatGain, sc.screenGain))
                             ? 1 : 0);
                    const std::int32_t nextIndirectCount =
                        indirectCount + ((isCombatUnit && !tmpl.canMoveAndFire && tmpl.maxRange > 1) ? 1 : 0);
                    const std::int32_t nextCapperCount = capperCount + (tmpl.canCapture ? 1 : 0);
                    const MilliFunds nextAntiAir = antiAirCap + (templateAntiAirCapable(ti) ? sc.cost : 0);
                    const MilliFunds stopVal = todayVal + evaluateTomorrowBest(
                        allOwnSiteIndices,
                        std::max<MilliFunds>(0, rem + m_input.nextTurnIncome - m_input.nextTurnRepairBill),
                        cov, claims, directCap, directVehicles, combatCount, screenBodies, indirectCount, capperCount,
                        antiAirCap);
                    const MilliFunds takeVal = todayVal + sc.utility + evaluateTomorrowBest(
                        allOwnSiteIndices,
                        std::max<MilliFunds>(0, rem - sc.cost + m_input.nextTurnIncome - m_input.nextTurnRepairBill),
                        nextCov, trialClaims, nextDirect, nextDirectVehicles, nextCombatCount, nextBodies,
                        nextIndirectCount, nextCapperCount, nextAntiAir);
                    if (forcedPos != pos && takeVal <= stopVal)
                        return false;
                    rem -= sc.cost;
                    todayVal += sc.utility;
                    cov = std::move(nextCov);
                    claims = std::move(trialClaims);
                    directCap = nextDirect;
                    directVehicles = nextDirectVehicles;
                    combatCount = nextCombatCount;
                    screenBodies = nextBodies;
                    indirectCount = nextIndirectCount;
                    capperCount = nextCapperCount;
                    antiAirCap = nextAntiAir;
                    used[static_cast<std::size_t>(pos)] = 1;
                    orders.push_back(ProductionBuildOrder{
                        .tile = m_input.sites[static_cast<std::size_t>(siteIdx)].tile,
                        .unitId = tmpl.unitId,
                        .templateIndex = ti,
                        .cost = sc.cost,
                        .utility = sc.utility,
                        .capperGain = sc.capperGain,
                        .combatGain = sc.combatGain,
                        .arrivalTurns = sc.arrivalTurns,
                    });
                    return true;
                };

                if (forcedPos >= 0 && forcedTmpl >= 0)
                {
                    if (!tryAdd(forcedPos, forcedTmpl))
                        return;
                }
                for (;;)
                {
                    int bestPos = -1;
                    int bestTmpl = -1;
                    double bestKey = -1.0;
                    MilliFunds bestRaw = 0;
                    for (int p = 0; p < static_cast<int>(openSiteIndices.size()); ++p)
                    {
                        if (used[static_cast<std::size_t>(p)])
                            continue;
                        const int siteIdx = openSiteIndices[static_cast<std::size_t>(p)];
                        for (const int ti : m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices)
                        {
                            if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                                continue;
                            if (m_input.templates[static_cast<std::size_t>(ti)].cost > rem)
                                continue;
                            const auto sc = scoreSiteTemplate(
                                siteIdx, ti, 0, cov, claims, directCap, directVehicles, combatCount, screenBodies,
                                indirectCount, capperCount, false, antiAirCap);
                            if (sc.utility <= 0)
                                continue;
                            const double key = packByRawUtility
                                ? static_cast<double>(sc.utility)
                                : static_cast<double>(sc.utility) /
                                      static_cast<double>(std::max<MilliFunds>(1, sc.cost));
                            if (key > bestKey || (key == bestKey && sc.utility > bestRaw))
                            {
                                bestKey = key;
                                bestRaw = sc.utility;
                                bestPos = p;
                                bestTmpl = ti;
                            }
                        }
                    }
                    if (bestPos < 0 || !tryAdd(bestPos, bestTmpl))
                        break;
                }
                const MilliFunds tomorrowVal = evaluateTomorrowBest(
                    allOwnSiteIndices,
                    std::max<MilliFunds>(0, rem + m_input.nextTurnIncome - m_input.nextTurnRepairBill),
                    cov, claims, directCap, directVehicles, combatCount, screenBodies, indirectCount, capperCount,
                    antiAirCap);
                const MilliFunds totalVal = todayVal + tomorrowVal;
                if (totalVal > bestPlan.totalValue ||
                    (totalVal == bestPlan.totalValue && todayVal > bestPlan.todayUtility))
                {
                    bestPlan.orders = std::move(orders);
                    bestPlan.todayUtility = todayVal;
                    bestPlan.tomorrowUtility = tomorrowVal;
                    bestPlan.totalValue = totalVal;
                    bestPlan.spentFunds = spendableFunds - rem;
                    bestPlan.remainingFunds = rem;
                }
            };

            evalCandidate(-1, -1, false);
            evalCandidate(-1, -1, true);
            // Also seed with the single highest-value unit per site so big Airport/Port/Heavy builds are never missed.
            for (int p = 0; p < static_cast<int>(openSiteIndices.size()); ++p)
            {
                const int siteIdx = openSiteIndices[static_cast<std::size_t>(p)];
                int topTmpl = -1;
                MilliFunds topUtil = 0;
                for (const int ti : m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices)
                {
                    if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                        continue;
                    if (m_input.templates[static_cast<std::size_t>(ti)].cost > spendableFunds)
                        continue;
                    std::vector<std::int32_t> tmpClaims = baseClaimed;
                    const auto sc = scoreSiteTemplate(
                        siteIdx, ti, 0, baseCoverage, tmpClaims, m_input.friendlyDirectCapital,
                        baseDirectVehicles, baseCombatCount, baseBodies, m_input.friendlyIndirectCount,
                        m_input.friendlyCapperCount, false);
                    if (sc.utility > topUtil)
                    {
                        topUtil = sc.utility;
                        topTmpl = ti;
                    }
                }
                if (topTmpl >= 0)
                {
                    evalCandidate(p, topTmpl, false);
                    evalCandidate(p, topTmpl, true);
                }
            }

            // Joint coordinate-ascent slot refinement across all open factories under today + tomorrow continuation.
            std::vector<int> chosenTmplPerPos(openSiteIndices.size(), -1);
            std::vector<MilliFunds> slotCosts(openSiteIndices.size(), 0);
            for (const auto & ord : bestPlan.orders)
            {
                for (std::size_t pos = 0; pos < openSiteIndices.size(); ++pos)
                {
                    const int siteIdx = openSiteIndices[pos];
                    if (m_input.sites[static_cast<std::size_t>(siteIdx)].tile == ord.tile)
                    {
                        chosenTmplPerPos[pos] = ord.templateIndex;
                        slotCosts[pos] = ord.cost;
                        break;
                    }
                }
            }
            constexpr std::size_t MAX_COORDINATE_ASCENT_PASSES = 4;
            bool improved = true;
            for (std::size_t pass = 0; improved && pass < MAX_COORDINATE_ASCENT_PASSES; ++pass)
            {
                improved = false;
                for (std::size_t pos = 0; pos < openSiteIndices.size(); ++pos)
                {
                    pumpUiEventsIfDue();
                    const int origTi = chosenTmplPerPos[pos];
                    const int siteIdx = openSiteIndices[pos];
                    const MilliFunds slotBudget = bestPlan.remainingFunds + slotCosts[pos];
                    bool slotImproved = false;
                    std::vector<int> bestSlotChoices = chosenTmplPerPos;
                    ProductionPlan bestSlotPlan = bestPlan;

                    std::vector<int> candidateTis{-1};
                    for (const int ti : m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices)
                    {
                        if (ti >= 0 && ti < static_cast<int>(m_input.templates.size()) &&
                            m_input.templates[static_cast<std::size_t>(ti)].cost <= slotBudget &&
                            std::ranges::find(candidateTis, ti) == candidateTis.end())
                        {
                            candidateTis.push_back(ti);
                        }
                    }
                    for (const int candTi : candidateTis)
                    {
                        if (candTi == origTi)
                            continue;
                        chosenTmplPerPos[pos] = candTi;
                        ProductionPlan trialPlan;
                        trialPlan.saveOnlyValue = bestPlan.saveOnlyValue;
                        if (evaluateOrderAssignment(openSiteIndices, chosenTmplPerPos, allOwnSiteIndices,
                                                  spendableFunds, baseCoverage, baseClaimed, trialPlan))
                        {
                            if (trialPlan.totalValue > bestSlotPlan.totalValue ||
                                (trialPlan.totalValue == bestSlotPlan.totalValue &&
                                 trialPlan.todayUtility > bestSlotPlan.todayUtility))
                            {
                                slotImproved = true;
                                bestSlotChoices = chosenTmplPerPos;
                                bestSlotPlan = std::move(trialPlan);
                            }
                        }
                        // When downsizing frees cash, also test greedily filling any remaining empty slots.
                        const MilliFunds candCost = candTi >= 0
                            ? m_input.templates[static_cast<std::size_t>(candTi)].cost : 0;
                        if (candCost < slotCosts[pos])
                        {
                            std::vector<int> refilled = chosenTmplPerPos;
                            MilliFunds rRem = spendableFunds;
                            std::vector<double> rCov = baseCoverage;
                            std::vector<std::int32_t> rClaims = baseClaimed;
                            MilliFunds rDirect = m_input.friendlyDirectCapital;
                            std::int32_t rDirectVeh = baseDirectVehicles;
                            std::int32_t rCombat = baseCombatCount;
                            std::int32_t rScreens = baseBodies;
                            std::int32_t rIndirect = m_input.friendlyIndirectCount;
                            std::int32_t rCapper = m_input.friendlyCapperCount;
                            MilliFunds rAntiAir = m_input.friendlyAntiAirCapital;
                            bool validPrefix = true;
                            for (std::size_t k = 0; k < openSiteIndices.size(); ++k)
                            {
                                if (refilled[k] < 0)
                                    continue;
                                const int ti = refilled[k];
                                const auto & tm = m_input.templates[static_cast<std::size_t>(ti)];
                                if (tm.cost <= 0 || tm.cost > rRem)
                                {
                                    validPrefix = false;
                                    break;
                                }
                                const auto sc = scoreSiteTemplate(
                                    openSiteIndices[k], ti, 0, rCov, rClaims, rDirect, rDirectVeh,
                                    rCombat, rScreens, rIndirect, rCapper, true, rAntiAir);
                                if (sc.utility <= 0)
                                {
                                    validPrefix = false;
                                    break;
                                }
                                rRem -= sc.cost;
                                if (sc.combatGain >= sc.capperGain)
                                    applyClaimedCoverage(ti, sc.claimedCoverage, rCov);
                                const bool isDV = tm.hasWeapons && tm.canMoveAndFire && !tm.canCapture;
                                if (isDV)
                                {
                                    rDirect += sc.cost;
                                    ++rDirectVeh;
                                }
                                if (tm.hasWeapons)
                                {
                                    ++rCombat;
                                    if (templateAntiAirCapable(ti))
                                        rAntiAir += sc.cost;
                                }
                                if (tm.canCapture)
                                {
                                    ++rCapper;
                                    if (tm.canMoveAndFire && sc.capperGain < std::max(sc.combatGain, sc.screenGain))
                                        ++rScreens;
                                }
                                if (tm.hasWeapons && !tm.canMoveAndFire && tm.maxRange > 1)
                                    ++rIndirect;
                            }
                            if (validPrefix && rRem > 0)
                            {
                                for (std::size_t k = 0; k < openSiteIndices.size(); ++k)
                                {
                                    if (refilled[k] >= 0)
                                        continue;
                                    int pickTi = -1;
                                    double pickKey = -1.0;
                                    CandidateScore pickSc;
                                    const int sIdx = openSiteIndices[k];
                                    for (const int ti : m_input.sites[static_cast<std::size_t>(sIdx)].buildableTemplateIndices)
                                    {
                                        if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                                            continue;
                                        if (m_input.templates[static_cast<std::size_t>(ti)].cost > rRem)
                                            continue;
                                        const auto sc = scoreSiteTemplate(
                                            sIdx, ti, 0, rCov, rClaims, rDirect, rDirectVeh, rCombat, rScreens,
                                            rIndirect, rCapper, false, rAntiAir);
                                        if (sc.utility <= 0)
                                            continue;
                                        const double key = static_cast<double>(sc.utility) /
                                            static_cast<double>(std::max<MilliFunds>(1, sc.cost));
                                        if (key > pickKey || (key == pickKey && sc.utility > pickSc.utility))
                                        {
                                            pickKey = key;
                                            pickSc = sc;
                                            pickTi = ti;
                                        }
                                    }
                                    if (pickTi >= 0)
                                    {
                                        const auto committed = scoreSiteTemplate(
                                            sIdx, pickTi, 0, rCov, rClaims, rDirect, rDirectVeh, rCombat, rScreens,
                                            rIndirect, rCapper, true, rAntiAir);
                                        if (committed.utility > 0 && committed.cost <= rRem)
                                        {
                                            refilled[k] = pickTi;
                                            rRem -= committed.cost;
                                            if (committed.combatGain >= committed.capperGain)
                                                applyClaimedCoverage(pickTi, committed.claimedCoverage, rCov);
                                            const auto & tm = m_input.templates[static_cast<std::size_t>(pickTi)];
                                            const bool isDV = tm.hasWeapons && tm.canMoveAndFire && !tm.canCapture;
                                            if (isDV)
                                            {
                                                rDirect += committed.cost;
                                                ++rDirectVeh;
                                            }
                                            if (tm.hasWeapons)
                                            {
                                                ++rCombat;
                                                if (templateAntiAirCapable(pickTi))
                                                    rAntiAir += committed.cost;
                                            }
                                            if (tm.canCapture)
                                            {
                                                ++rCapper;
                                                if (tm.canMoveAndFire &&
                                                    committed.capperGain < std::max(committed.combatGain, committed.screenGain))
                                                    ++rScreens;
                                            }
                                            if (tm.hasWeapons && !tm.canMoveAndFire && tm.maxRange > 1)
                                                ++rIndirect;
                                        }
                                    }
                                }
                                ProductionPlan refilledPlan;
                                refilledPlan.saveOnlyValue = bestPlan.saveOnlyValue;
                                if (evaluateOrderAssignment(openSiteIndices, refilled, allOwnSiteIndices,
                                                          spendableFunds, baseCoverage, baseClaimed, refilledPlan) &&
                                    (refilledPlan.totalValue > bestSlotPlan.totalValue ||
                                     (refilledPlan.totalValue == bestSlotPlan.totalValue &&
                                      refilledPlan.todayUtility > bestSlotPlan.todayUtility)))
                                {
                                    slotImproved = true;
                                    bestSlotChoices = std::move(refilled);
                                    bestSlotPlan = std::move(refilledPlan);
                                }
                            }
                        }
                        chosenTmplPerPos[pos] = origTi;
                    }
                    if (slotImproved)
                    {
                        chosenTmplPerPos = std::move(bestSlotChoices);
                        for (std::size_t k = 0; k < openSiteIndices.size(); ++k)
                        {
                            slotCosts[k] = chosenTmplPerPos[k] >= 0
                                ? m_input.templates[static_cast<std::size_t>(chosenTmplPerPos[k])].cost : 0;
                        }
                        bestPlan = std::move(bestSlotPlan);
                        improved = true;
                    }
                }
            }
        }

        ProductionModelInput m_input;
        std::vector<MilliFunds> m_enemyPerTurnThreat;
        std::vector<std::vector<MilliFunds>> m_enemyThreatFromTurn;
        std::vector<std::vector<std::int32_t>> m_combatMoveTurns;
        std::vector<std::vector<std::int32_t>> m_enemyForwardCost;
        std::int32_t m_totalFrontDirectSlots{0};
        std::int32_t m_liveGroundEnemyCount{0};
        std::map<int, std::vector<std::vector<std::int32_t>>> m_siteForwardCost;
        mutable std::map<std::tuple<int, int, int>, std::vector<std::int32_t>> m_tileForwardCost;
        std::map<int, std::vector<std::vector<std::int32_t>>> m_siteToPropCost;
        std::map<int, std::vector<std::vector<std::int32_t>>> m_propToPropCost;
        mutable std::map<std::tuple<int, std::int32_t, std::int32_t, std::size_t>, std::vector<std::int32_t>>
            m_enemyFireRingCost;
        std::map<int, MilliFunds> m_captureSitePerTurnSurplus;
    };
}
