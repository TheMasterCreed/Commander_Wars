#include "ai/coordinator/unifiedproduction.h"
#include "ai/coordinator/bundlebuilder.h"
#include <bit>
#include <unordered_map>

namespace Coordinator
{
    namespace
    {
        constexpr std::size_t PROD_HASH_MIX = 0x9e3779b97f4a7c15ULL;
        constexpr std::size_t PROD_HASH_LEFT_SHIFT = 6;
        constexpr std::size_t PROD_TOMORROW_CACHE_INITIAL_BUCKETS = 1024;
        constexpr std::size_t PROD_PATH_CACHE_INITIAL_BUCKETS = 256;
        constexpr std::size_t PROD_SCORE_CACHE_INITIAL_BUCKETS = 2048;

        inline void prodHashCombine(std::size_t & seed, std::size_t v) noexcept
        {
            seed ^= v + PROD_HASH_MIX + (seed << PROD_HASH_LEFT_SHIFT) + (seed >> 2);
        }

        struct CapperCacheKey
        {
            int siteIdx{0};
            int tmplIdx{0};
            std::int32_t spawnTurn{0};
            bool hasCombatPresence{false};
            std::vector<std::int32_t> claims;
            friend bool operator==(const CapperCacheKey &, const CapperCacheKey &) = default;
        };
        struct CapperCacheKeyHash
        {
            std::size_t operator()(const CapperCacheKey & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(k.siteIdx) * PROD_HASH_MIX;
                prodHashCombine(h, static_cast<std::size_t>(k.tmplIdx));
                prodHashCombine(h, static_cast<std::size_t>(k.spawnTurn));
                prodHashCombine(h, static_cast<std::size_t>(k.hasCombatPresence));
                for (const std::int32_t c : k.claims)
                    prodHashCombine(h, static_cast<std::size_t>(c));
                return h;
            }
        };
        struct CapperCacheVal
        {
            MilliFunds gain{0};
            std::vector<std::int32_t> outClaims;
        };

        struct CombatCacheKey
        {
            int siteIdx{0};
            int tmplIdx{0};
            std::int32_t spawnTurn{0};
            bool indirectScreened{false};
            std::int32_t effDirectVehicles{0};
            std::int32_t effIndirectCap{0};
            std::int32_t effIndirectCount{0};
            MilliFunds effAntiAirCap{0};
            std::vector<std::uint64_t> coverageBits;
            friend bool operator==(const CombatCacheKey &, const CombatCacheKey &) = default;
        };
        struct CombatCacheKeyHash
        {
            std::size_t operator()(const CombatCacheKey & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(k.siteIdx) * PROD_HASH_MIX;
                prodHashCombine(h, static_cast<std::size_t>(k.tmplIdx));
                prodHashCombine(h, static_cast<std::size_t>(k.spawnTurn));
                prodHashCombine(h, static_cast<std::size_t>(k.indirectScreened));
                prodHashCombine(h, static_cast<std::size_t>(k.effDirectVehicles));
                prodHashCombine(h, static_cast<std::size_t>(k.effIndirectCap));
                prodHashCombine(h, static_cast<std::size_t>(k.effIndirectCount));
                prodHashCombine(h, static_cast<std::size_t>(k.effAntiAirCap));
                for (const std::uint64_t b : k.coverageBits)
                    prodHashCombine(h, static_cast<std::size_t>(b));
                return h;
            }
        };

        struct ProdTileKey
        {
            int gridId{0};
            int x{0};
            int y{0};
            friend bool operator==(const ProdTileKey &, const ProdTileKey &) = default;
        };
        struct ProdTileKeyHash
        {
            std::size_t operator()(const ProdTileKey & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(k.gridId) * PROD_HASH_MIX;
                prodHashCombine(h, static_cast<std::size_t>(k.x));
                prodHashCombine(h, static_cast<std::size_t>(k.y));
                return h;
            }
        };

        struct ProdFireRingKey
        {
            int gridId{0};
            std::int32_t minR{0};
            std::int32_t maxR{0};
            std::size_t enemyIdx{0};
            friend bool operator==(const ProdFireRingKey &, const ProdFireRingKey &) = default;
        };
        struct ProdFireRingKeyHash
        {
            std::size_t operator()(const ProdFireRingKey & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(k.gridId) * PROD_HASH_MIX;
                prodHashCombine(h, static_cast<std::size_t>(k.minR));
                prodHashCombine(h, static_cast<std::size_t>(k.maxR));
                prodHashCombine(h, k.enemyIdx);
                return h;
            }
        };
    } // namespace

    struct UnifiedProductionModel::FastCache
    {
        struct TomorrowCacheKeyHash
        {
            std::size_t operator()(const TomorrowCacheKey & k) const noexcept
            {
                std::size_t h = static_cast<std::size_t>(k.siteCount) * PROD_HASH_MIX;
                prodHashCombine(h, static_cast<std::size_t>(k.firstSite));
                prodHashCombine(h, static_cast<std::size_t>(k.tomorrowFunds));
                prodHashCombine(h, static_cast<std::size_t>(k.directCapital));
                prodHashCombine(h, static_cast<std::size_t>(k.directVehicles));
                prodHashCombine(h, static_cast<std::size_t>(k.combatCount));
                prodHashCombine(h, static_cast<std::size_t>(k.screenBodies));
                prodHashCombine(h, static_cast<std::size_t>(k.screenVehicles));
                prodHashCombine(h, static_cast<std::size_t>(k.indirectCount));
                prodHashCombine(h, static_cast<std::size_t>(k.capperCount));
                prodHashCombine(h, static_cast<std::size_t>(k.antiAirCapital));
                for (const std::int32_t v : k.quantizedCoverage)
                    prodHashCombine(h, static_cast<std::size_t>(v));
                for (const std::int32_t c : k.claims)
                    prodHashCombine(h, static_cast<std::size_t>(c));
                return h;
            }
        };

        struct FactoryCounterEntry
        {
            std::int32_t sReachTurns{0};
            MilliFunds airAaThreshold{-1};
            bool skippedWhenScreened{false};
            double rawExcess{0.0};
        };

        struct IndirectZoneEntry
        {
            double fullHorizonStake{0.0};
            std::int32_t bestCostScreened{std::numeric_limits<std::int32_t>::max()};
            std::int32_t bestCostUnscreened{std::numeric_limits<std::int32_t>::max()};
            std::vector<std::size_t> occupyingEnemies;
        };

        struct ScreenThreatStep
        {
            MilliFunds cost{0};
            MilliFunds threat{0};
        };

        std::size_t siteCount{0};
        std::size_t tmplCount{0};
        std::size_t enemyCount{0};
        std::int32_t ownSiteCount{0};
        MilliFunds liveDirectEnemyCap{0};
        std::vector<std::vector<std::vector<std::int32_t>>> siteForwardByGrid;
        std::vector<std::vector<std::vector<std::int32_t>>> siteToPropByGrid;
        std::vector<std::vector<std::vector<std::int32_t>>> propToPropByGrid;
        std::vector<std::vector<std::int32_t>> enemyAttackTurnsGrid;
        std::vector<std::int32_t> siteToEnemyMoveTurns;
        std::vector<TilePoint> siteToEnemyBestSq;
        std::vector<std::uint8_t> enemyDefendsProp;
        std::vector<MilliFunds> siteMinDirectVehicleCost;
        std::vector<std::uint8_t> tmplAntiAirCapable;
        std::vector<std::int32_t> heavyGroundEnemyCountByTmpl;
        std::vector<std::int32_t> groundEnemyStrikersByTmpl;
        std::vector<std::vector<ScreenThreatStep>> screenThreatBySiteEnemy;
        std::vector<std::vector<FactoryCounterEntry>> factoryCounters;
        std::vector<std::vector<IndirectZoneEntry>> indirectZoneTargets;
        std::unordered_map<TomorrowCacheKey, MilliFunds, TomorrowCacheKeyHash> tomorrowCache;
        std::unordered_map<ProdTileKey, std::vector<std::int32_t>, ProdTileKeyHash> tileForwardCost;
        std::unordered_map<ProdFireRingKey, std::vector<std::int32_t>, ProdFireRingKeyHash> enemyFireRingCost;
        std::unordered_map<CapperCacheKey, CapperCacheVal, CapperCacheKeyHash> capperCache;
        std::unordered_map<CombatCacheKey, CombatEvalResult, CombatCacheKeyHash> combatCache;

        static std::vector<CandidateScore> selectParetoFrontier(
            std::span<const CandidateScore> scored, const std::vector<ProductionUnitTemplate> & templates)
        {
            std::vector<CandidateScore> capperScored;
            std::vector<CandidateScore> directScored;
            std::vector<CandidateScore> indirectScored;
            for (const auto & sc : scored)
            {
                const auto & tm = templates[static_cast<std::size_t>(sc.templateIndex)];
                if (tm.canCapture) capperScored.push_back(sc);
                else if (tm.canMoveAndFire) directScored.push_back(sc);
                else indirectScored.push_back(sc);
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
            std::vector<CandidateScore> dirPareto = paretoOf(directScored);
            std::vector<CandidateScore> indPareto = paretoOf(indirectScored);
            std::vector<CandidateScore> combined;
            combined.reserve(capPareto.size() + dirPareto.size() + indPareto.size());
            for (const auto & sc : dirPareto) combined.push_back(sc);
            for (const auto & sc : indPareto) combined.push_back(sc);
            for (const auto & sc : capPareto) combined.push_back(sc);
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
                    if (capped.size() >= UNIFIED_PROD_PARETO_PER_SITE) break;
                    capped.push_back(combined[idx]);
                }
                combined = std::move(capped);
            }
            return combined;
        }
    };

    UnifiedProductionModel::UnifiedProductionModel(ProductionModelInput input)
        : m_input(std::move(input))
    {
        precompute();
    }

    bool UnifiedProductionModel::valid() const
    {
        return m_input.width > 0 && m_input.height > 0 && m_input.horizonTurns > 0;
    }

    std::optional<EnemySiteCounterThreat> UnifiedProductionModel::bestEnemyCounterFromSite(TilePoint enemySite, int friendlyKi) const
    {
        const auto it = m_input.enemySiteCounters.find({enemySite.x, enemySite.y, friendlyKi});
        if (it == m_input.enemySiteCounters.end())
            return std::nullopt;
        return it->second;
    }

    MilliFunds UnifiedProductionModel::enemySiteBlockDenial(TilePoint enemySite) const
    {
        const auto it = m_input.enemySiteBlockValue.find({enemySite.x, enemySite.y});
        if (it == m_input.enemySiteBlockValue.end())
            return 0;
        return std::max<MilliFunds>(0, it->second);
    }

    MilliFunds UnifiedProductionModel::ownStrategicCaptureLoss(TilePoint ownTile, std::int32_t horizon) const
    {
        const auto it = m_input.ownStrategicLoss.find({ownTile.x, ownTile.y});
        if (it == m_input.ownStrategicLoss.end() || it->second <= 0 || horizon <= 1)
            return 0;
        return (it->second * std::max<std::int32_t>(1, horizon - 1)) / std::max<std::int32_t>(1, horizon);
    }

    MilliFunds UnifiedProductionModel::enemyCaptureInterruptStrategicValue(TilePoint tile, std::int32_t delayTurns,
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

    ProductionPlan UnifiedProductionModel::planBuilds(std::span<const TilePoint> openFriendlySites,
                          MilliFunds spendableFunds,
                          std::span<const std::int32_t> claimedTurnOverride) const
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
            m_input.friendlyCombatUnitCount, initialScreens(), m_input.friendlyIndirectCount,
            m_input.friendlyCapperCount);
        result.saveOnlyValue = saveOnlyTomorrow;
        result.tomorrowUtility = saveOnlyTomorrow;
        result.totalValue = saveOnlyTomorrow;
        result.choseSave = true;

        if (openSiteIndices.empty() || spendableFunds <= 0)
            return result;

        // Sort open sites by fastest combat/capture arrival, breaking ties in favor of ground sites over air-only sites.
        std::ranges::sort(openSiteIndices, [&](int a, int b) {
            return std::tuple{siteMinArrival(a), siteBuildsOnlyAir(a), a} <
                std::tuple{siteMinArrival(b), siteBuildsOnlyAir(b), b};
        });

        const bool exhaustive = openSiteIndices.size() <= UNIFIED_PROD_EXHAUSTIVE_SITE_LIMIT &&
            affordableSearchFits(openSiteIndices, spendableFunds);
        if (exhaustive || openSiteIndices.size() <= UNIFIED_PROD_EXACT_SITE_LIMIT)
        {
            searchExactToday(openSiteIndices, allOwnSiteIndices, spendableFunds, baseCoverage, baseClaimed, result, exhaustive);
        }
        else
        {
            searchGreedyWithBigAlternatives(
                openSiteIndices, allOwnSiteIndices, spendableFunds, baseCoverage, baseClaimed, result);
        }

        result.choseSave = result.orders.empty() && result.saveOnlyValue > 0;
        return result;
    }

    void UnifiedProductionModel::carryPurchasesForward(ProductionModelInput & next, const ProductionPlan & plan) const
    {
        auto state = initialProductionState(plan.remainingFunds + plan.spentFunds, buildInitialCoverage(), buildClaimedTurns({}));
        for (const auto & order : plan.orders)
        {
            const int site = findSiteIndex(order.tile, OwnerSign::Ours);
            const auto sc = quotePurchase(site, order.templateIndex, 0, state, true);
            advancePurchase(state, sc);
            if (m_input.templates[order.templateIndex].hasWeapons)
                next.totalFriendlyCombatCapital += sc.survivingCapital;
        }
        next.friendlyDirectCapital = state.directCapital;
        next.friendlyDirectVehicleCount = state.directVehicles;
        next.friendlyCombatUnitCount = state.combatCount;
        next.friendlyScreenBodies = state.screens.additionalBodies;
        next.friendlyVehicleScreens = state.screens.assignedVehicles;
        next.friendlyIndirectCount = state.indirectCount;
        next.friendlyCapperCount = state.capperCount;
        next.friendlyAntiAirCapital = state.antiAirCapital;
        for (std::size_t i = 0; i < state.coverage.size(); ++i)
            next.enemies[i].initialFriendlyCoverage = state.coverage[i];
        for (std::size_t i = 0; i < state.claims.size(); ++i)
            next.properties[i].claimedTurn = state.claims[i];
    }

    MilliFunds UnifiedProductionModel::marginalOpenSitesValue(std::span<const TilePoint> openFriendlySites,
                                  MilliFunds spendableFunds,
                                  std::span<const std::int32_t> claimedTurnOverride) const
    {
        if (openFriendlySites.empty() || spendableFunds <= 0 || !valid())
            return 0;
        const ProductionPlan plan = planBuilds(openFriendlySites, spendableFunds, claimedTurnOverride);
        return std::max<MilliFunds>(0, plan.totalValue - plan.saveOnlyValue);
    }

    MilliFunds UnifiedProductionModel::zeroCostSpawnHorizonValue(TilePoint siteTile, int templateIndex) const
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
            effectiveDirectVehicles(), m_input.friendlyCombatUnitCount, initialScreens(),
            m_input.friendlyIndirectCount, m_input.friendlyCapperCount, false);
        return tmpl.cost + sc.utility;
    }

    MilliFunds UnifiedProductionModel::siteCaptureProductionSurplus(TilePoint siteTile, std::int32_t captureTurn,
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

    MilliFunds UnifiedProductionModel::combatBonusCaptureValue(TilePoint tile, std::int32_t ownTurn, std::int32_t horizon) const
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

    std::vector<std::pair<TilePoint, OwnerSign>> UnifiedProductionModel::strategicCaptureTiles() const
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

    double UnifiedProductionModel::defenderTerrainFactor(const ProductionEnemyUnit & enemy, double defenderHp)
    {
        if (enemy.bypassesGroundScreen || enemy.defenseStars <= 0)
            return 1.0;
        const double hpFrac = std::clamp(defenderHp, 0.0, UNIFIED_PROD_FULL_HP) / UNIFIED_PROD_FULL_HP;
        const double defPct = static_cast<double>(enemy.defenseStars) * UNIFIED_PROD_DEFENSE_PER_STAR_PCT * hpFrac;
        return std::max(UNIFIED_PROD_MIN_DEFENSE_FACTOR,
                        (UNIFIED_PROD_PERCENT_SCALE - defPct) / UNIFIED_PROD_PERCENT_SCALE);
    }

    double UnifiedProductionModel::firstStrikeNetTrade(const ProductionUnitTemplate & unit, const ProductionEnemyUnit & enemy,
                                  double baseDmg, double returnDmg, std::int32_t fireDist)
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

    double UnifiedProductionModel::enemyFirstStrikeNetThreat(const ProductionUnitTemplate & unit, const ProductionEnemyUnit & enemy,
                                        double enemyStrikeDmg, double ourCounterDmg,
                                        std::int32_t fireDist)
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

    bool UnifiedProductionModel::tileEnterable(const MobilityCostGrid & grid, TilePoint t)
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

    std::vector<std::int32_t> UnifiedProductionModel::buildReverseDistances(int gridId, std::span<const TilePoint> targets,
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

    std::vector<std::int32_t> UnifiedProductionModel::buildForwardDistances(int gridId, TilePoint origin) const
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

    void UnifiedProductionModel::precompute()
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
            const auto siteFwdIt = m_siteForwardCost.find(g);
            for (std::size_t si = 0; si < siteCount; ++si)
            {
                std::vector<std::int32_t> localFwd;
                const std::vector<std::int32_t> * fwdPtr = nullptr;
                if (siteFwdIt != m_siteForwardCost.end() && si < siteFwdIt->second.size())
                    fwdPtr = &siteFwdIt->second[si];
                else
                {
                    localFwd = buildForwardDistances(g, m_input.sites[si].tile);
                    fwdPtr = &localFwd;
                }
                const auto & fwd = *fwdPtr;
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

        // 2b. Populate flat O(1) lookup tables in FastCache before running steady-state site surplus queries.
        auto fast = std::make_shared<FastCache>();
        fast->siteCount = siteCount;
        fast->tmplCount = tmplCount;
        fast->enemyCount = m_input.enemies.size();
        fast->tomorrowCache.reserve(PROD_TOMORROW_CACHE_INITIAL_BUCKETS);
        fast->tileForwardCost.reserve(PROD_PATH_CACHE_INITIAL_BUCKETS);
        fast->enemyFireRingCost.reserve(PROD_PATH_CACHE_INITIAL_BUCKETS);
        fast->capperCache.reserve(PROD_SCORE_CACHE_INITIAL_BUCKETS);
        fast->combatCache.reserve(PROD_SCORE_CACHE_INITIAL_BUCKETS);
        for (const auto & s : m_input.sites)
        {
            if (s.owner == OwnerSign::Ours)
                ++fast->ownSiteCount;
        }
        const std::size_t gridCount = m_input.grids.size();
        fast->siteForwardByGrid.resize(gridCount);
        for (const auto & [g, vec] : m_siteForwardCost)
        {
            if (g >= 0 && static_cast<std::size_t>(g) < gridCount)
                fast->siteForwardByGrid[static_cast<std::size_t>(g)] = vec;
        }
        fast->siteToPropByGrid.resize(gridCount);
        for (const auto & [g, vec] : m_siteToPropCost)
        {
            if (g >= 0 && static_cast<std::size_t>(g) < gridCount)
                fast->siteToPropByGrid[static_cast<std::size_t>(g)] = vec;
        }
        fast->propToPropByGrid.resize(gridCount);
        for (const auto & [g, vec] : m_propToPropCost)
        {
            if (g >= 0 && static_cast<std::size_t>(g) < gridCount)
                fast->propToPropByGrid[static_cast<std::size_t>(g)] = vec;
        }
        const std::size_t cellCount =
            static_cast<std::size_t>(m_input.width) * static_cast<std::size_t>(m_input.height);
        fast->enemyAttackTurnsGrid.resize(m_input.enemies.size());
        std::vector<std::int32_t> ringBestCost;
        for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
        {
            const auto & e = m_input.enemies[ei];
            if (e.tile == INVALID_TILE || cellCount == 0)
                continue;
            auto & outGrid = fast->enemyAttackTurnsGrid[ei];
            outGrid.assign(cellCount, UNIFIED_PROD_UNREACHABLE_TURNS);
            const std::int32_t minR = std::max<std::int32_t>(1, e.minRange);
            const std::int32_t maxR = std::max<std::int32_t>(minR, e.maxRange);
            if (e.canMoveAndFire && e.movementPoints > 0 &&
                ei < m_enemyForwardCost.size() && !m_enemyForwardCost[ei].empty())
            {
                const auto & fwd = m_enemyForwardCost[ei];
                ringBestCost.assign(cellCount, std::numeric_limits<std::int32_t>::max());
                for (int sy = 0; sy < m_input.height; ++sy)
                {
                    for (int sx = 0; sx < m_input.width; ++sx)
                    {
                        const std::size_t sIdx =
                            static_cast<std::size_t>(sy) * static_cast<std::size_t>(m_input.width) +
                            static_cast<std::size_t>(sx);
                        const std::int32_t c = fwd[sIdx];
                        if (c == std::numeric_limits<std::int32_t>::max())
                            continue;
                        const int ty0 = std::max(0, sy - maxR);
                        const int ty1 = std::min(m_input.height - 1, sy + maxR);
                        for (int ty = ty0; ty <= ty1; ++ty)
                        {
                            const int dy = std::abs(ty - sy);
                            const int remX = maxR - dy;
                            const int tx0 = std::max(0, sx - remX);
                            const int tx1 = std::min(m_input.width - 1, sx + remX);
                            for (int tx = tx0; tx <= tx1; ++tx)
                            {
                                const int d = std::abs(tx - sx) + dy;
                                if (d < minR)
                                    continue;
                                const std::size_t tIdx =
                                    static_cast<std::size_t>(ty) * static_cast<std::size_t>(m_input.width) +
                                    static_cast<std::size_t>(tx);
                                if (c < ringBestCost[tIdx])
                                    ringBestCost[tIdx] = c;
                            }
                        }
                    }
                }
                for (std::size_t cIdx = 0; cIdx < cellCount; ++cIdx)
                {
                    if (ringBestCost[cIdx] < std::numeric_limits<std::int32_t>::max())
                        outGrid[cIdx] = e.spawnDelayTurns + (ringBestCost[cIdx] + e.movementPoints - 1) / e.movementPoints;
                }
            }
            const int ty0 = std::max(0, e.tile.y - maxR);
            const int ty1 = std::min(m_input.height - 1, e.tile.y + maxR);
            for (int ty = ty0; ty <= ty1; ++ty)
            {
                const int dy = std::abs(ty - e.tile.y);
                const int remX = maxR - dy;
                const int tx0 = std::max(0, e.tile.x - remX);
                const int tx1 = std::min(m_input.width - 1, e.tile.x + remX);
                for (int tx = tx0; tx <= tx1; ++tx)
                {
                    if (std::abs(tx - e.tile.x) + dy < minR)
                        continue;
                    const std::size_t tIdx =
                        static_cast<std::size_t>(ty) * static_cast<std::size_t>(m_input.width) +
                        static_cast<std::size_t>(tx);
                    outGrid[tIdx] = e.spawnDelayTurns;
                }
            }
        }
        m_fast = fast;

        fast->siteToEnemyMoveTurns.assign(siteCount * tmplCount * m_input.enemies.size(), UNIFIED_PROD_UNREACHABLE_TURNS);
        fast->siteToEnemyBestSq.assign(siteCount * tmplCount * m_input.enemies.size(), INVALID_TILE);
        for (std::size_t si = 0; si < siteCount; ++si)
        {
            for (std::size_t ti = 0; ti < tmplCount; ++ti)
            {
                const auto & tmpl = m_input.templates[ti];
                const int effGrid = (tmpl.gridId >= 0 && tmpl.gridId < static_cast<int>(gridCount))
                    ? tmpl.gridId : (!m_input.grids.empty() ? 0 : -1);
                const std::vector<std::int32_t> * exactFwd =
                    (tmpl.gridId >= 0 && static_cast<std::size_t>(tmpl.gridId) < fast->siteForwardByGrid.size() &&
                     si < fast->siteForwardByGrid[static_cast<std::size_t>(tmpl.gridId)].size())
                    ? &fast->siteForwardByGrid[static_cast<std::size_t>(tmpl.gridId)][si]
                    : nullptr;
                const std::vector<std::int32_t> * effFwd =
                    (effGrid >= 0 && static_cast<std::size_t>(effGrid) < fast->siteForwardByGrid.size() &&
                     si < fast->siteForwardByGrid[static_cast<std::size_t>(effGrid)].size())
                    ? &fast->siteForwardByGrid[static_cast<std::size_t>(effGrid)][si]
                    : nullptr;
                const int minR = std::max<std::int32_t>(1, tmpl.minRange);
                const int maxR = std::max<std::int32_t>(minR, tmpl.maxRange);
                for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
                {
                    const TilePoint enemyTile = m_input.enemies[ei].tile;
                    if (enemyTile == INVALID_TILE)
                        continue;
                    const std::size_t flat = (si * tmplCount + ti) * m_input.enemies.size() + ei;
                    const int x0 = std::max(0, enemyTile.x - maxR);
                    const int x1 = std::min(m_input.width - 1, enemyTile.x + maxR);
                    const int y0 = std::max(0, enemyTile.y - maxR);
                    const int y1 = std::min(m_input.height - 1, enemyTile.y + maxR);
                    if (tmpl.movementPoints > 0 && exactFwd != nullptr && !exactFwd->empty())
                    {
                        std::int32_t bestC = std::numeric_limits<std::int32_t>::max();
                        for (int y = y0; y <= y1; ++y)
                        {
                            for (int x = x0; x <= x1; ++x)
                            {
                                const int d = manhattanDistance({x, y}, enemyTile);
                                if (d < minR || d > maxR) continue;
                                const std::int32_t c = (*exactFwd)[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_input.width) + static_cast<std::size_t>(x)];
                                if (c < bestC) bestC = c;
                            }
                        }
                        if (bestC < std::numeric_limits<std::int32_t>::max())
                            fast->siteToEnemyMoveTurns[flat] = (bestC + tmpl.movementPoints - 1) / tmpl.movementPoints;
                    }
                    if (effFwd != nullptr && !effFwd->empty())
                    {
                        std::int32_t bestC = std::numeric_limits<std::int32_t>::max();
                        TilePoint bestT = INVALID_TILE;
                        for (int y = y0; y <= y1; ++y)
                        {
                            for (int x = x0; x <= x1; ++x)
                            {
                                const int d = manhattanDistance({x, y}, enemyTile);
                                if (d < minR || d > maxR) continue;
                                const std::size_t idx = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_input.width) + static_cast<std::size_t>(x);
                                if (idx < effFwd->size() && (*effFwd)[idx] < bestC)
                                {
                                    bestC = (*effFwd)[idx];
                                    bestT = {x, y};
                                }
                            }
                        }
                        fast->siteToEnemyBestSq[flat] = bestT;
                    }
                }
            }
        }

        fast->enemyDefendsProp.assign(propCount, 0);
        for (std::size_t p = 0; p < propCount; ++p)
        {
            const auto & prop = m_input.properties[p];
            if (prop.owner != OwnerSign::Enemy)
                continue;
            for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
            {
                const auto & e = m_input.enemies[ei];
                if (e.hp <= 0.0 || e.isProjected || e.tile == INVALID_TILE)
                    continue;
                if (e.tile == prop.tile ||
                    (!e.canCapture && e.replacementCost > 0 && enemyAttackTurnsToTile(ei, prop.tile) <= 1))
                {
                    fast->enemyDefendsProp[p] = 1;
                    break;
                }
            }
        }

        fast->siteMinDirectVehicleCost.assign(siteCount, 0);
        for (std::size_t si = 0; si < siteCount; ++si)
        {
            MilliFunds minC = 0;
            for (const int ti : m_input.sites[si].buildableTemplateIndices)
            {
                if (ti < 0 || ti >= static_cast<int>(tmplCount)) continue;
                const auto & t = m_input.templates[static_cast<std::size_t>(ti)];
                if (t.hasWeapons && t.canMoveAndFire && !t.canCapture && t.cost > 0 && (minC == 0 || t.cost < minC))
                    minC = t.cost;
            }
            fast->siteMinDirectVehicleCost[si] = minC;
        }

        fast->tmplAntiAirCapable.assign(tmplCount, 0);
        for (std::size_t ti = 0; ti < tmplCount; ++ti)
        {
            const auto & tmpl = m_input.templates[ti];
            if (!tmpl.hasWeapons || tmpl.cost <= 0) continue;
            bool aa = false;
            for (std::size_t j = 0; j < tmplCount && j < tmpl.baseDamageVsTemplate.size() && !aa; ++j)
                if (m_input.templates[j].bypassesGroundDefense && tmpl.baseDamageVsTemplate[j] > 0.0) aa = true;
            for (std::size_t ei = 0; ei < m_input.enemies.size() && ei < tmpl.baseDamageVsEnemy.size() && !aa; ++ei)
                if (m_input.enemies[ei].bypassesGroundScreen && tmpl.baseDamageVsEnemy[ei] > 0.0) aa = true;
            fast->tmplAntiAirCapable[ti] = aa ? 1 : 0;
        }

        fast->liveDirectEnemyCap = m_input.enemyDirectCapital;
        if (fast->liveDirectEnemyCap <= 0)
        {
            for (const auto & enemy : m_input.enemies)
            {
                if (enemy.hp > 0.0 && enemy.canMoveAndFire && !enemy.bypassesGroundScreen &&
                    !enemy.canCapture && !enemy.isProjected)
                {
                    fast->liveDirectEnemyCap += static_cast<MilliFunds>(std::llround(
                        static_cast<double>(enemy.replacementCost) * (enemy.hp / UNIFIED_PROD_FULL_HP)));
                }
            }
        }
        fast->heavyGroundEnemyCountByTmpl.assign(tmplCount, 0);
        fast->groundEnemyStrikersByTmpl.assign(tmplCount, 0);
        for (std::size_t ti = 0; ti < tmplCount; ++ti)
        {
            const auto & tmpl = m_input.templates[ti];
            std::int32_t heavy = 0;
            std::int32_t strikers = 0;
            for (const auto & enemy : m_input.enemies)
            {
                if (enemy.hp > 0.0 && enemy.canMoveAndFire && !enemy.bypassesGroundScreen)
                {
                    if (!enemy.canCapture && enemy.replacementCost >= tmpl.cost * 2)
                        ++heavy;
                    if (!(enemy.canCapture && enemy.replacementCost <= tmpl.cost) && enemy.spawnDelayTurns <= 0)
                        ++strikers;
                }
            }
            fast->heavyGroundEnemyCountByTmpl[ti] = heavy;
            fast->groundEnemyStrikersByTmpl[ti] = strikers;
        }

        fast->screenThreatBySiteEnemy.resize(siteCount * m_input.enemies.size());
        for (std::size_t si = 0; si < siteCount; ++si)
        {
            for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
            {
                const auto & enemy = m_input.enemies[ei];
                if (enemy.hp <= 0.0 || !enemy.canMoveAndFire || enemy.bypassesGroundScreen || enemy.spawnDelayTurns > 0)
                    continue;
                ProductionEnemyUnit fullEnemy = enemy;
                fullEnemy.hp = UNIFIED_PROD_FULL_HP;
                std::vector<FastCache::ScreenThreatStep> raw;
                raw.push_back({0, ei < m_enemyPerTurnThreat.size() ? m_enemyPerTurnThreat[ei] : 0});
                for (const int pti : m_input.sites[si].buildableTemplateIndices)
                {
                    if (pti < 0 || pti >= static_cast<int>(tmplCount)) continue;
                    const auto & pTmpl = m_input.templates[static_cast<std::size_t>(pti)];
                    if (pTmpl.canCapture || !pTmpl.hasWeapons) continue;
                    if (!pTmpl.canMoveAndFire && m_input.friendlyIndirectCount <= 0) continue;
                    const double eStrikeP = ei < pTmpl.enemyFirstStrikeVsMe.size() ? pTmpl.enemyFirstStrikeVsMe[ei] : 0.0;
                    const double cDmgP = ei < pTmpl.counterDamageWhenStruck.size() ? pTmpl.counterDamageWhenStruck[ei] : 0.0;
                    const double exchangeLoss = enemyFirstStrikeNetThreat(pTmpl, fullEnemy, eStrikeP, cDmgP);
                    const double pDealtHp = eStrikeP > 0.0
                        ? std::min(UNIFIED_PROD_FULL_HP, eStrikeP / UNIFIED_PROD_HP_PERCENT_PER_STEP) : 0.0;
                    const double pAtk = ei < pTmpl.baseDamageVsEnemy.size() ? pTmpl.baseDamageVsEnemy[ei] : 0.0;
                    const double pRet = ei < pTmpl.returnDamageFromEnemy.size() ? pTmpl.returnDamageFromEnemy[ei] : 0.0;
                    const double lostFirstStrikeSwing = pDealtHp > 0.0
                        ? (pDealtHp / UNIFIED_PROD_FULL_HP) * firstStrikeNetTrade(pTmpl, fullEnemy, pAtk, pRet) : 0.0;
                    const MilliFunds t = static_cast<MilliFunds>(std::llround(exchangeLoss + lostFirstStrikeSwing));
                    raw.push_back({pTmpl.cost, t});
                }
                std::ranges::sort(raw, [](const auto & a, const auto & b) {
                    return a.cost != b.cost ? a.cost < b.cost : a.threat > b.threat;
                });
                auto & pared = fast->screenThreatBySiteEnemy[si * m_input.enemies.size() + ei];
                MilliFunds runMax = -1;
                for (const auto & step : raw)
                {
                    if (step.threat > runMax)
                    {
                        pared.push_back(step);
                        runMax = step.threat;
                    }
                }
            }
        }

        // Precompute static enemy-factory counter risk entries per (siteIdx, tmplIdx).
        fast->factoryCounters.resize(siteCount * tmplCount);
        for (std::size_t si = 0; si < siteCount; ++si)
        {
            for (std::size_t ti = 0; ti < tmplCount; ++ti)
            {
                const auto & tmpl = m_input.templates[ti];
                if (!tmpl.hasWeapons || tmpl.movementPoints <= 0)
                    continue;
                const std::vector<std::int32_t> * fwdPtr =
                    (tmpl.gridId >= 0 && static_cast<std::size_t>(tmpl.gridId) < fast->siteForwardByGrid.size() &&
                     si < fast->siteForwardByGrid[static_cast<std::size_t>(tmpl.gridId)].size())
                    ? &fast->siteForwardByGrid[static_cast<std::size_t>(tmpl.gridId)][si]
                    : nullptr;
                auto & entries = fast->factoryCounters[si * tmplCount + ti];
                for (int eSiteIdx = 0; eSiteIdx < static_cast<int>(siteCount); ++eSiteIdx)
                {
                    const auto & eSite = m_input.sites[static_cast<std::size_t>(eSiteIdx)];
                    if (eSite.owner != OwnerSign::Enemy)
                        continue;
                    std::int32_t sReachTurns = UNIFIED_PROD_UNREACHABLE_TURNS;
                    if (fwdPtr != nullptr && eSite.tile.x >= 0 && eSite.tile.x < m_input.width &&
                        eSite.tile.y >= 0 && eSite.tile.y < m_input.height)
                    {
                        const std::size_t cell =
                            static_cast<std::size_t>(eSite.tile.y) * static_cast<std::size_t>(m_input.width) +
                            static_cast<std::size_t>(eSite.tile.x);
                        if (cell < fwdPtr->size() && (*fwdPtr)[cell] != std::numeric_limits<std::int32_t>::max())
                            sReachTurns = ((*fwdPtr)[cell] + std::max<std::int32_t>(1, tmpl.movementPoints) - 1) /
                                std::max<std::int32_t>(1, tmpl.movementPoints);
                    }
                    if (sReachTurns >= UNIFIED_PROD_UNREACHABLE_TURNS || 1 + sReachTurns >= H)
                        continue;
                    const MilliFunds siteBudget =
                        eSite.ownerAvailableFunds > 0 ? eSite.ownerAvailableFunds : m_input.nextTurnIncome;
                    const std::int32_t ourCorridorTurns = siteToSiteAttackTurns(static_cast<int>(si), eSiteIdx, tmpl);
                    for (const int cti : eSite.buildableTemplateIndices)
                    {
                        if (cti < 0 || cti >= static_cast<int>(tmplCount)) continue;
                        const auto & ct = m_input.templates[static_cast<std::size_t>(cti)];
                        const bool withinEqualCost = ct.cost <= tmpl.cost;
                        const bool affordableHardCounter = ct.bypassesGroundDefense == tmpl.bypassesGroundDefense &&
                            ct.cost <= tmpl.cost * 2 &&
                            m_input.nextTurnIncome > 0 && ct.cost <= m_input.nextTurnIncome;
                        if (!ct.hasWeapons || !ct.canMoveAndFire || ct.canCapture || ct.cost <= 0 ||
                            (!withinEqualCost && !affordableHardCounter))
                            continue;
                        const bool isAirVsGround = !tmpl.bypassesGroundDefense && ct.bypassesGroundDefense;
                        if (isAirVsGround && m_input.enemyAirCapital <= 0)
                            continue;
                        if (siteBudget > 0 && ct.cost > siteBudget)
                            continue;
                        const std::int32_t ctCorridorTurns = siteToSiteAttackTurns(eSiteIdx, static_cast<int>(si), ct);
                        if (ctCorridorTurns >= UNIFIED_PROD_UNREACHABLE_TURNS)
                            continue;
                        if (ti >= ct.baseDamageVsTemplate.size() ||
                            static_cast<std::size_t>(cti) >= tmpl.baseDamageVsTemplate.size())
                            continue;
                        const double ctStrike = ct.baseDamageVsTemplate[ti];
                        if (ctStrike <= 0.0) continue;
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
                        const double myTrade = firstStrikeNetTrade(tmpl, ctEnemy, myDmg, ctRet, tmpl.minRange);
                        if (!withinEqualCost && myTrade > 0.0) continue;
                        if (tmpl.canMoveAndFire && ctCorridorTurns > ourCorridorTurns && myTrade > 0.0) continue;
                        const double rawExcess = std::max(
                            0.0, enemyFirstStrikeNetThreat(tmpl, ctEnemy, ctStrike, myCounter, ct.minRange) -
                                     std::max(0.0, myTrade));
                        if (rawExcess <= 0.0) continue;
                        entries.push_back({
                            sReachTurns,
                            isAirVsGround ? (m_input.enemyAirCapital + ct.cost / 2) : -1,
                            !ct.bypassesGroundDefense && ct.maxRange <= 1,
                            rawExcess
                        });
                    }
                }
            }
        }

        // Precompute static indirect zone-denial targets and ring costs per (siteIdx, tmplIdx).
        fast->indirectZoneTargets.resize(siteCount * tmplCount);
        if (!m_input.properties.empty() || !m_input.ownStrategicLoss.empty())
        {
            const std::int32_t capTurns = std::max<std::int32_t>(
                1, (m_input.capturePointsToCapture + static_cast<std::int32_t>(UNIFIED_PROD_FULL_HP) - 1) /
                       static_cast<std::int32_t>(UNIFIED_PROD_FULL_HP));
            struct RawZoneTarget
            {
                TilePoint tile{INVALID_TILE};
                double neutralStake{0.0};
                MilliFunds ownLoss{0};
                std::vector<std::size_t> occupyingEnemies;
            };
            std::vector<RawZoneTarget> contestedTargets;
            const auto addIfContested = [&](TilePoint targetTile, double neutralStake, MilliFunds ownLoss, std::int32_t maxCapDist) {
                if (targetTile == INVALID_TILE) return;
                bool contested = false;
                std::vector<std::size_t> occ;
                for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
                {
                    const auto & e = m_input.enemies[ei];
                    if (e.hp <= 0.0 || e.tile == INVALID_TILE) continue;
                    if (e.tile == targetTile)
                        occ.push_back(ei);
                    if (e.canCapture && enemyAttackTurnsToTile(ei, targetTile) <= maxCapDist)
                        contested = true;
                }
                if (contested)
                    contestedTargets.push_back({targetTile, neutralStake, ownLoss, std::move(occ)});
            };
            for (const auto & prop : m_input.properties)
            {
                if (prop.income <= 0 || prop.owner != OwnerSign::Neutral) continue;
                const double stake =
                    static_cast<double>(prop.income * static_cast<MilliFunds>(H)) / static_cast<double>(capTurns);
                addIfContested(prop.tile, stake, 0, 2);
            }
            for (const auto & [coords, ownLoss] : m_input.ownStrategicLoss)
            {
                if (ownLoss <= 0) continue;
                addIfContested({coords.first, coords.second}, 0.0, ownLoss, 1);
            }
            if (!contestedTargets.empty())
            {
                std::vector<std::size_t> groundDirectEnemies;
                for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
                {
                    const auto & e = m_input.enemies[ei];
                    if (e.hp > 0.0 && e.canMoveAndFire && !e.bypassesGroundScreen && e.maxRange <= 1 && !e.canCapture)
                        groundDirectEnemies.push_back(ei);
                }
                const auto tileSafeUnscreened = [&](TilePoint sq, TilePoint targetTile) -> bool {
                    if (groundDirectEnemies.empty()) return false;
                    for (const std::size_t ei : groundDirectEnemies)
                    {
                        const std::int32_t toTarget = enemyAttackTurnsToTile(ei, targetTile);
                        const std::int32_t toSq = enemyAttackTurnsToTile(ei, sq);
                        if (toSq <= 1) return false;
                        if (toTarget < UNIFIED_PROD_UNREACHABLE_TURNS && toSq < toTarget + 2) return false;
                    }
                    return true;
                };
                for (std::size_t ti = 0; ti < tmplCount; ++ti)
                {
                    const auto & tmpl = m_input.templates[ti];
                    if (!tmpl.hasWeapons || tmpl.canMoveAndFire || tmpl.maxRange <= 1 || tmpl.movementPoints <= 0)
                        continue;
                    if (tmpl.gridId < 0 || static_cast<std::size_t>(tmpl.gridId) >= fast->siteForwardByGrid.size())
                        continue;
                    const auto & gridFwd = fast->siteForwardByGrid[static_cast<std::size_t>(tmpl.gridId)];
                    const std::int32_t minR = std::max<std::int32_t>(1, tmpl.minRange);
                    const std::int32_t maxR = std::max<std::int32_t>(minR, tmpl.maxRange);
                    for (std::size_t si = 0; si < siteCount && si < gridFwd.size(); ++si)
                    {
                        const auto & fwd = gridFwd[si];
                        auto & zoneList = fast->indirectZoneTargets[si * tmplCount + ti];
                        for (const auto & rt : contestedTargets)
                        {
                            const double stake = rt.ownLoss > 0
                                ? static_cast<double>(std::min(rt.ownLoss, tmpl.cost)) / static_cast<double>(capTurns)
                                : rt.neutralStake;
                            if (stake <= 0.0) continue;
                            const int x0 = std::max(0, rt.tile.x - maxR);
                            const int x1 = std::min(m_input.width - 1, rt.tile.x + maxR);
                            const int y0 = std::max(0, rt.tile.y - maxR);
                            const int y1 = std::min(m_input.height - 1, rt.tile.y + maxR);
                            std::int32_t bestScreened = std::numeric_limits<std::int32_t>::max();
                            std::int32_t bestUnscreened = std::numeric_limits<std::int32_t>::max();
                            for (int y = y0; y <= y1; ++y)
                            {
                                for (int x = x0; x <= x1; ++x)
                                {
                                    const int d = manhattanDistance({x, y}, rt.tile);
                                    if (d < minR || d > maxR) continue;
                                    const std::size_t cell =
                                        static_cast<std::size_t>(y) * static_cast<std::size_t>(m_input.width) +
                                        static_cast<std::size_t>(x);
                                    if (cell >= fwd.size()) continue;
                                    const std::int32_t fc = fwd[cell];
                                    if (fc < bestScreened) bestScreened = fc;
                                    if (fc < bestUnscreened && tileSafeUnscreened({x, y}, rt.tile))
                                        bestUnscreened = fc;
                                }
                            }
                            if (bestScreened < std::numeric_limits<std::int32_t>::max() ||
                                bestUnscreened < std::numeric_limits<std::int32_t>::max())
                            {
                                zoneList.push_back({stake, bestScreened, bestUnscreened, rt.occupyingEnemies});
                            }
                        }
                    }
                }
            }
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
            const auto baseBodies = initialScreens();
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
        // Clear caches populated during precompute before m_captureSitePerTurnSurplus was finalized.
        if (m_fast)
        {
            m_fast->capperCache.clear();
            m_fast->tomorrowCache.clear();
        }
    }

    int UnifiedProductionModel::findSiteIndex(TilePoint tile, OwnerSign owner) const
    {
        for (int i = 0; i < static_cast<int>(m_input.sites.size()); ++i)
        {
            const auto & s = m_input.sites[static_cast<std::size_t>(i)];
            if (s.tile == tile && s.owner == owner)
                return i;
        }
        return -1;
    }

    bool UnifiedProductionModel::siteBuildsOnlyAir(int siteIdx) const
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

    std::int32_t UnifiedProductionModel::siteMinArrival(int siteIdx) const
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

    std::vector<std::int32_t> UnifiedProductionModel::buildClaimedTurns(std::span<const std::int32_t> overrideTurns) const
    {
        std::vector<std::int32_t> claimed(m_input.properties.size(), m_input.horizonTurns);
        for (std::size_t i = 0; i < m_input.properties.size(); ++i)
        {
            claimed[i] = (i < overrideTurns.size()) ? overrideTurns[i] : m_input.properties[i].claimedTurn;
        }
        return claimed;
    }

    std::vector<double> UnifiedProductionModel::buildInitialCoverage() const
    {
        std::vector<double> cov(m_input.enemies.size(), 0.0);
        for (std::size_t i = 0; i < m_input.enemies.size(); ++i)
            cov[i] = std::max(0.0, m_input.enemies[i].initialFriendlyCoverage);
        return cov;
    }

    std::int32_t UnifiedProductionModel::travelTurnsSiteToProp(int siteIdx, int propIdx, const ProductionUnitTemplate & tmpl) const
    {
        if (tmpl.movementPoints <= 0)
            return UNIFIED_PROD_UNREACHABLE_TURNS;
        if (m_fast && tmpl.gridId >= 0 &&
            static_cast<std::size_t>(tmpl.gridId) < m_fast->siteToPropByGrid.size())
        {
            const auto & gridTable = m_fast->siteToPropByGrid[static_cast<std::size_t>(tmpl.gridId)];
            if (static_cast<std::size_t>(siteIdx) < gridTable.size())
            {
                const std::int32_t cost = gridTable[static_cast<std::size_t>(siteIdx)][static_cast<std::size_t>(propIdx)];
                if (cost == std::numeric_limits<std::int32_t>::max())
                    return UNIFIED_PROD_UNREACHABLE_TURNS;
                return (cost + tmpl.movementPoints - 1) / tmpl.movementPoints;
            }
        }
        if (const auto it = m_siteToPropCost.find(tmpl.gridId); it != m_siteToPropCost.end())
        {
            const std::int32_t cost = it->second[static_cast<std::size_t>(siteIdx)][static_cast<std::size_t>(propIdx)];
            if (cost == std::numeric_limits<std::int32_t>::max())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            return (cost + tmpl.movementPoints - 1) / tmpl.movementPoints;
        }
        return UNIFIED_PROD_UNREACHABLE_TURNS;
    }

    std::int32_t UnifiedProductionModel::travelTurnsPropToProp(int pA, int pB, const ProductionUnitTemplate & tmpl) const
    {
        if (tmpl.movementPoints <= 0)
            return UNIFIED_PROD_UNREACHABLE_TURNS;
        if (m_fast && tmpl.gridId >= 0 &&
            static_cast<std::size_t>(tmpl.gridId) < m_fast->propToPropByGrid.size())
        {
            const auto & gridTable = m_fast->propToPropByGrid[static_cast<std::size_t>(tmpl.gridId)];
            if (static_cast<std::size_t>(pA) < gridTable.size())
            {
                const std::int32_t cost = gridTable[static_cast<std::size_t>(pA)][static_cast<std::size_t>(pB)];
                if (cost == std::numeric_limits<std::int32_t>::max())
                    return UNIFIED_PROD_UNREACHABLE_TURNS;
                return (cost + tmpl.movementPoints - 1) / tmpl.movementPoints;
            }
        }
        if (const auto it = m_propToPropCost.find(tmpl.gridId); it != m_propToPropCost.end())
        {
            const std::int32_t cost = it->second[static_cast<std::size_t>(pA)][static_cast<std::size_t>(pB)];
            if (cost == std::numeric_limits<std::int32_t>::max())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            return (cost + tmpl.movementPoints - 1) / tmpl.movementPoints;
        }
        return UNIFIED_PROD_UNREACHABLE_TURNS;
    }

    MilliFunds UnifiedProductionModel::propertyStreamValue(const ProductionCapturableProperty & prop, std::int32_t ownTurn) const
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

    std::int32_t UnifiedProductionModel::effectiveDirectVehicles() const
    {
        if (m_input.friendlyDirectVehicleCount > 0)
            return m_input.friendlyDirectVehicleCount;
        if (m_input.friendlyDirectCapital > 0)
            return std::max<std::int32_t>(1, m_input.friendlyCombatUnitCount - m_input.friendlyIndirectCount);
        return 0;
    }

    UnifiedProductionModel::ScreenInventory UnifiedProductionModel::initialScreens() const
    {
        return {effectiveScreenBodies(), m_input.friendlyVehicleScreens};
    }

    std::int32_t UnifiedProductionModel::effectiveScreenBodies() const
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

    std::int32_t UnifiedProductionModel::enemyAttackTurnsToTile(std::size_t ei, TilePoint target) const
    {
        if (ei >= m_input.enemies.size() || target == INVALID_TILE)
            return UNIFIED_PROD_UNREACHABLE_TURNS;
        if (m_fast && ei < m_fast->enemyAttackTurnsGrid.size())
        {
            const auto & g = m_fast->enemyAttackTurnsGrid[ei];
            if (!g.empty() && target.x >= 0 && target.x < m_input.width &&
                target.y >= 0 && target.y < m_input.height)
            {
                return g[static_cast<std::size_t>(target.y) * static_cast<std::size_t>(m_input.width) +
                         static_cast<std::size_t>(target.x)];
            }
            if (g.empty())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
        }
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

    TilePoint UnifiedProductionModel::bestFiringSquareToEnemy(int siteIdx, bool fromSite, TilePoint fromTile,
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
            if (m_fast)
            {
                const ProdTileKey key{effGrid, fromTile.x, fromTile.y};
                auto it = m_fast->tileForwardCost.find(key);
                if (it == m_fast->tileForwardCost.end())
                    it = m_fast->tileForwardCost.emplace(key, buildForwardDistances(effGrid, fromTile)).first;
                fwdPtr = &it->second;
            }
            else
            {
                const auto key = std::tuple{effGrid, fromTile.x, fromTile.y};
                auto it = m_tileForwardCost.find(key);
                if (it == m_tileForwardCost.end())
                    it = m_tileForwardCost.emplace(key, buildForwardDistances(effGrid, fromTile)).first;
                fwdPtr = &it->second;
            }
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

    std::int32_t UnifiedProductionModel::siteToSiteAttackTurns(int fromSiteIdx, int toSiteIdx,
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

    std::int32_t UnifiedProductionModel::effectiveEnemyDirectCount() const
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

    std::int32_t UnifiedProductionModel::moveTurnsSiteToEnemy(int siteIdx, const ProductionUnitTemplate & tmpl,
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

    std::int32_t UnifiedProductionModel::moveTurnsTileToEnemy(TilePoint origin, const ProductionUnitTemplate & tmpl,
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
            const std::vector<std::int32_t> * distPtr = nullptr;
            if (m_fast)
            {
                const ProdFireRingKey key{tmpl.gridId, minR, maxR, enemyIdx};
                auto it = m_fast->enemyFireRingCost.find(key);
                if (it == m_fast->enemyFireRingCost.end())
                {
                    it = m_fast->enemyFireRingCost.emplace(
                        key, buildReverseDistances(tmpl.gridId, std::span<const TilePoint>(&enemyTile, 1), minR, maxR)).first;
                }
                distPtr = &it->second;
            }
            else
            {
                const auto key = std::tuple{tmpl.gridId, minR, maxR, enemyIdx};
                auto it = m_enemyFireRingCost.find(key);
                if (it == m_enemyFireRingCost.end())
                {
                    it = m_enemyFireRingCost.emplace(
                        key, buildReverseDistances(tmpl.gridId, std::span<const TilePoint>(&enemyTile, 1), minR, maxR)).first;
                }
                distPtr = &it->second;
            }
            const std::size_t cell =
                static_cast<std::size_t>(origin.y) * static_cast<std::size_t>(m_input.width) +
                static_cast<std::size_t>(origin.x);
            const std::int32_t cost = cell < distPtr->size()
                ? (*distPtr)[cell] : std::numeric_limits<std::int32_t>::max();
            if (cost == std::numeric_limits<std::int32_t>::max())
                return UNIFIED_PROD_UNREACHABLE_TURNS;
            return (cost + tmpl.movementPoints - 1) / tmpl.movementPoints;
        }
        return UNIFIED_PROD_UNREACHABLE_TURNS;
    }

    MilliFunds UnifiedProductionModel::evaluateCapperGain(int siteIdx, int tmplIdx, std::int32_t spawnTurn, bool hasCombatPresence,
                              std::int32_t friendlyCapperCount,
                              std::vector<std::int32_t> & claims) const
    {
        (void)friendlyCapperCount;
        const auto & tmpl = m_input.templates[static_cast<std::size_t>(tmplIdx)];
        if (!tmpl.canCapture || tmpl.captureRate <= 0 || tmpl.movementPoints <= 0 || m_input.properties.empty())
            return 0;
        const std::int32_t horizon = m_input.horizonTurns;
        const std::int32_t capTurns =
            (m_input.capturePointsToCapture + tmpl.captureRate - 1) / tmpl.captureRate;

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
                const bool enemyDefendsProp = (m_fast && static_cast<std::size_t>(p) < m_fast->enemyDefendsProp.size())
                    ? (m_fast->enemyDefendsProp[static_cast<std::size_t>(p)] != 0)
                    : (prop.owner == OwnerSign::Enemy && [&]() {
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
                      }());
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

    MilliFunds UnifiedProductionModel::evaluateScreenGain(int siteIdx, int tmplIdx, std::int32_t spawnTurn,
                              MilliFunds friendlyDirectCapital, std::int32_t friendlyDirectVehicles,
                              std::int32_t friendlyIndirectCount,
                              std::int32_t friendlyScreenBodies) const
    {
        const auto & tmpl = m_input.templates[static_cast<std::size_t>(tmplIdx)];
        if (!tmpl.canMoveAndFire || tmpl.movementPoints <= 0 || tmpl.cost <= 0)
            return 0;
        const MilliFunds twoWaveBudget = std::max<MilliFunds>(
            0, m_input.currentFunds + m_input.nextTurnIncome - m_input.nextTurnRepairBill);
        MilliFunds minDirectVehicleCost = (m_fast && static_cast<std::size_t>(siteIdx) < m_fast->siteMinDirectVehicleCost.size())
            ? m_fast->siteMinDirectVehicleCost[static_cast<std::size_t>(siteIdx)] : 0;
        if (!m_fast)
        {
            for (const int ti : m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices)
            {
                if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                    continue;
                const auto & t = m_input.templates[static_cast<std::size_t>(ti)];
                if (t.hasWeapons && t.canMoveAndFire && !t.canCapture && t.cost > 0 &&
                    (minDirectVehicleCost == 0 || t.cost < minDirectVehicleCost))
                {
                    minDirectVehicleCost = t.cost;
                }
            }
        }
        const bool incomingArmor = (friendlyDirectVehicles <= 0 && spawnTurn == 0 &&
                                    minDirectVehicleCost > 0 && m_input.currentFunds >= minDirectVehicleCost);
        if (friendlyDirectVehicles <= 0 && !incomingArmor)
            return 0;
        const MilliFunds curPerUnitCap = friendlyDirectVehicles > 0
            ? std::max<MilliFunds>(1, friendlyDirectCapital / friendlyDirectVehicles)
            : minDirectVehicleCost;
        // Keep Wave-0 and Wave-1 screen anchors time-consistent so delaying an Infantry to Wave 1 never invents extra screen value.
        const MilliFunds basePerUnitCap = (m_input.friendlyDirectCapital > 0 && effectiveDirectVehicles() > 0)
            ? std::max<MilliFunds>(1, m_input.friendlyDirectCapital / effectiveDirectVehicles())
            : minDirectVehicleCost;
        const MilliFunds perCombatUnitCap = (spawnTurn > 0 && basePerUnitCap > 0)
            ? std::min(curPerUnitCap, std::max(basePerUnitCap, minDirectVehicleCost))
            : curPerUnitCap;
        const MilliFunds effDirectCapital = friendlyDirectCapital > 0 ? friendlyDirectCapital : minDirectVehicleCost;
        if (tmpl.cost * 2 > effDirectCapital)
            return 0;

        std::int32_t groundEnemyStrikers =
            (m_fast && static_cast<std::size_t>(tmplIdx) < m_fast->groundEnemyStrikersByTmpl.size())
            ? m_fast->groundEnemyStrikersByTmpl[static_cast<std::size_t>(tmplIdx)] : 0;
        if (!m_fast)
        {
            for (const auto & enemy : m_input.enemies)
            {
                if (enemy.hp > 0.0 && enemy.canMoveAndFire && !enemy.bypassesGroundScreen &&
                    !(enemy.canCapture && enemy.replacementCost <= tmpl.cost) && enemy.spawnDelayTurns <= 0)
                {
                    ++groundEnemyStrikers;
                }
            }
        }
        if (groundEnemyStrikers <= 0)
            return 0;
        const std::int32_t horizon = std::max<std::int32_t>(1, m_input.horizonTurns);
        const std::int32_t neededScreens = std::min(groundEnemyStrikers, friendlyDirectVehicles + friendlyIndirectCount);
        // Front-line screens attrit across flanks: extra replacement bodies share marginal shield value, but still pay full screenBodyCost.
        if (friendlyScreenBodies >= neededScreens && tmpl.cost * 2 > perCombatUnitCap)
            return 0;
        const std::int32_t screenDenom = friendlyScreenBodies < neededScreens
            ? 1 : (1 + (friendlyScreenBodies - neededScreens));

        MilliFunds bestShieldSurplus = 0;
        const std::size_t enemyCount = m_input.enemies.size();
        const std::size_t siteTmplBase = (static_cast<std::size_t>(siteIdx) * m_input.templates.size() +
                                          static_cast<std::size_t>(tmplIdx)) * enemyCount;
        for (std::size_t ei = 0; ei < enemyCount; ++ei)
        {
            const auto & enemy = m_input.enemies[ei];
            if (enemy.hp <= 0.0 || !enemy.canMoveAndFire || enemy.bypassesGroundScreen ||
                (enemy.canCapture && enemy.replacementCost <= tmpl.cost) || enemy.spawnDelayTurns > 0)
                continue;
            const std::int32_t mt = (m_fast && siteTmplBase + ei < m_fast->siteToEnemyMoveTurns.size())
                ? m_fast->siteToEnemyMoveTurns[siteTmplBase + ei]
                : moveTurnsSiteToEnemy(siteIdx, tmpl, ei);
            const std::int32_t arriveTurn = spawnTurn + 1 + mt;
            if (mt >= UNIFIED_PROD_UNREACHABLE_TURNS || arriveTurn >= horizon)
                continue;

            MilliFunds protectedThreat = ei < m_enemyPerTurnThreat.size() ? m_enemyPerTurnThreat[ei] : 0;
            if (m_fast && static_cast<std::size_t>(siteIdx) * enemyCount + ei < m_fast->screenThreatBySiteEnemy.size())
            {
                for (const auto & step : m_fast->screenThreatBySiteEnemy[static_cast<std::size_t>(siteIdx) * enemyCount + ei])
                {
                    if (step.cost > perCombatUnitCap)
                        break;
                    protectedThreat = step.threat;
                }
            }
            else
            {
                ProductionEnemyUnit fullEnemy = enemy;
                fullEnemy.hp = UNIFIED_PROD_FULL_HP;
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
            }
            const double eStrikeMe = ei < tmpl.enemyFirstStrikeVsMe.size() ? tmpl.enemyFirstStrikeVsMe[ei] : 0.0;
            const double scaledMe = eStrikeMe;
            const double dealtMe = std::min(UNIFIED_PROD_FULL_HP, scaledMe / UNIFIED_PROD_HP_PERCENT_PER_STEP);
            const MilliFunds screenBodyCost = static_cast<MilliFunds>(std::llround(
                (static_cast<double>(tmpl.cost) * dealtMe) / UNIFIED_PROD_FULL_HP));
            const double remFrac = static_cast<double>(horizon - arriveTurn) / static_cast<double>(horizon);
            const MilliFunds marginalBenefit = static_cast<MilliFunds>(std::llround(
                (static_cast<double>(protectedThreat) / static_cast<double>(screenDenom)) * remFrac));
            if (marginalBenefit > screenBodyCost)
                bestShieldSurplus = std::max(bestShieldSurplus, marginalBenefit - screenBodyCost);
        }
        return bestShieldSurplus;
    }

    bool UnifiedProductionModel::allLiveEnemiesCovered(std::span<const double> coverage) const
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

    bool UnifiedProductionModel::templateAntiAirCapable(int tmplIdx) const
    {
        if (tmplIdx < 0 || tmplIdx >= static_cast<int>(m_input.templates.size()))
            return false;
        if (m_fast && static_cast<std::size_t>(tmplIdx) < m_fast->tmplAntiAirCapable.size())
            return m_fast->tmplAntiAirCapable[static_cast<std::size_t>(tmplIdx)] != 0;
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

    [[gnu::noinline]] UnifiedProductionModel::CombatEvalResult UnifiedProductionModel::evaluateCombatGain(
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
        MilliFunds liveDirectEnemyCap = m_fast ? m_fast->liveDirectEnemyCap : m_input.enemyDirectCapital;
        if (!m_fast && liveDirectEnemyCap <= 0)
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
        const std::int32_t totalScreenBodies = friendlyDirectVehicles + friendlyScreenBodies;
        std::int32_t heavyGroundEnemyCount =
            (m_fast && static_cast<std::size_t>(tmplIdx) < m_fast->heavyGroundEnemyCountByTmpl.size())
            ? m_fast->heavyGroundEnemyCountByTmpl[static_cast<std::size_t>(tmplIdx)] : 0;
        if (!m_fast)
        {
            for (const auto & enemy : m_input.enemies)
            {
                if (enemy.hp > 0.0 && enemy.canMoveAndFire && !enemy.bypassesGroundScreen &&
                    !enemy.canCapture && enemy.replacementCost >= tmpl.cost * 2)
                {
                    ++heavyGroundEnemyCount;
                }
            }
        }
        const std::size_t siteTmplEnemyBase =
            (static_cast<std::size_t>(siteIdx) * m_input.templates.size() + static_cast<std::size_t>(tmplIdx)) *
            m_input.enemies.size();
        const MilliFunds directScreenAnchor = std::max(tmpl.cost * 2, liveDirectEnemyCap);
        const MilliFunds fullScreenReq = static_cast<MilliFunds>(friendlyIndirectCount + 1) * directScreenAnchor;
        // On narrow chokepoints (<= 2 front slots), sealing the choke with >= 2x indirect cost unlocks the 1st indirect.
        const bool chokepointSealed = friendlyIndirectCount == 0 &&
            m_totalFrontDirectSlots > 0 && m_totalFrontDirectSlots <= 2 &&
            totalScreenBodies >= m_totalFrontDirectSlots &&
            friendlyDirectVehicles >= 1 &&
            friendlyDirectCapital >= tmpl.cost * 2;
        // Against enemy armor costing >= 2x our indirect, up to 1 screened indirect per heavy enemy unlocks without dollar-for-dollar direct parity.
        const bool heavyArmorScreened = friendlyIndirectCount < heavyGroundEnemyCount &&
            friendlyDirectVehicles >= 1 &&
            totalScreenBodies >= 2 * (friendlyIndirectCount + 1) &&
            friendlyDirectCapital >= tmpl.cost * static_cast<MilliFunds>(friendlyIndirectCount + 1);
        const bool openFieldScreened = friendlyDirectCapital > 0 &&
            friendlyDirectVehicles >= friendlyIndirectCount + 1 &&
            totalScreenBodies >= 2 * (friendlyIndirectCount + 1) &&
            friendlyDirectCapital >= fullScreenReq;
        const bool indirectProtectedByScreen = !tmpl.canMoveAndFire && tmpl.maxRange > 1 &&
            (chokepointSealed || heavyArmorScreened || openFieldScreened);
        CombatCacheKey combatKey;
        if (m_fast)
        {
            const bool isIndirect = !tmpl.canMoveAndFire && tmpl.maxRange > 1;
            const bool isGroundDirect = tmpl.canMoveAndFire && tmpl.minRange == 1 && !tmpl.bypassesGroundDefense;
            combatKey.siteIdx = siteIdx;
            combatKey.tmplIdx = tmplIdx;
            combatKey.spawnTurn = spawnTurn;
            combatKey.indirectScreened = indirectProtectedByScreen;
            combatKey.effDirectVehicles =
                (isGroundDirect && friendlyDirectVehicles > m_totalFrontDirectSlots) ? friendlyDirectVehicles : 0;
            combatKey.effIndirectCap = (isIndirect && !tmpl.bypassesGroundDefense)
                ? std::max<std::int32_t>(1, std::min(totalScreenBodies / 2, m_liveGroundEnemyCount))
                : 0;
            combatKey.effIndirectCount = isIndirect ? friendlyIndirectCount : 0;
            combatKey.effAntiAirCap =
                (m_input.enemyAirCapital > 0 && !tmpl.bypassesGroundDefense) ? friendlyAntiAirCapital : 0;
            combatKey.coverageBits.resize(coverage.size());
            for (std::size_t i = 0; i < coverage.size(); ++i)
                combatKey.coverageBits[i] = std::bit_cast<std::uint64_t>(coverage[i]);
            if (const auto it = m_fast->combatCache.find(combatKey); it != m_fast->combatCache.end())
                return it->second;
        }
        std::vector<std::uint8_t> engaged(static_cast<std::size_t>(enemyCount), 0);
        std::vector<std::pair<std::size_t, double>> claimedCoverage;

        std::int32_t curTurn = spawnTurn + 1;
        TilePoint curTile = m_input.sites[static_cast<std::size_t>(siteIdx)].tile;
        double curHp = UNIFIED_PROD_FULL_HP;
        std::int32_t remAmmo = tmpl.maxAmmo > 0 ? tmpl.maxAmmo : horizon;
        bool firstTarget = true;
        std::int32_t targetsEngaged = 0;
        std::int32_t firstFireTurn = UNIFIED_PROD_UNREACHABLE_TURNS;
        TilePoint firstEngageSq = INVALID_TILE;
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
            TilePoint bestEngageSq = INVALID_TILE;
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
                    ? ((m_fast && siteTmplEnemyBase + static_cast<std::size_t>(ei) < m_fast->siteToEnemyMoveTurns.size())
                           ? m_fast->siteToEnemyMoveTurns[siteTmplEnemyBase + static_cast<std::size_t>(ei)]
                           : moveTurnsSiteToEnemy(siteIdx, tmpl, static_cast<std::size_t>(ei)))
                    : moveTurnsTileToEnemy(curTile, tmpl, static_cast<std::size_t>(ei));
                if (moveTurns >= UNIFIED_PROD_UNREACHABLE_TURNS)
                    continue;
                // Strictly faster mobile enemies across another flank (moveTurns > 1) that lose the 1v1 front-shift away instead of queuing as Target 2+.
                if (!firstTarget && moveTurns > 1 && enemy.canMoveAndFire && enemy.movementPoints > tmpl.movementPoints &&
                    enemy.directAttackSlots > 1 && !enemy.canCapture)
                {
                    const double retCheck = static_cast<std::size_t>(ei) < tmpl.returnDamageFromEnemy.size()
                        ? tmpl.returnDamageFromEnemy[static_cast<std::size_t>(ei)] : 0.0;
                    const double eStrikeCheck = static_cast<std::size_t>(ei) < tmpl.enemyFirstStrikeVsMe.size()
                        ? tmpl.enemyFirstStrikeVsMe[static_cast<std::size_t>(ei)] : 0.0;
                    const double cCheck = static_cast<std::size_t>(ei) < tmpl.counterDamageWhenStruck.size()
                        ? tmpl.counterDamageWhenStruck[static_cast<std::size_t>(ei)] : 0.0;
                    if (firstStrikeNetTrade(tmpl, enemy, baseDmg, retCheck, tmpl.minRange) >
                        enemyFirstStrikeNetThreat(tmpl, enemy, eStrikeCheck, cCheck, enemy.minRange))
                    {
                        continue;
                    }
                }
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

                const TilePoint bestSq = (firstTarget && m_fast &&
                                          siteTmplEnemyBase + static_cast<std::size_t>(ei) < m_fast->siteToEnemyBestSq.size())
                    ? m_fast->siteToEnemyBestSq[siteTmplEnemyBase + static_cast<std::size_t>(ei)]
                    : bestFiringSquareToEnemy(
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
                        if (indirectProtectedByScreen && !amb.bypassesGroundScreen && amb.maxRange <= 1)
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
                                           (cPct * (afterUs / UNIFIED_PROD_FULL_HP) *
                                            defenderTerrainFactor(enemy, simEnemyHp)) /
                                               UNIFIED_PROD_HP_PERCENT_PER_STEP)
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
                auto applyCoveringHardCounter = [&](double & hp) {
                    if (hp <= 0.0 || bestSq == INVALID_TILE)
                        return;
                    double worstHardHit = 0.0;
                    for (int ej = 0; ej < enemyCount; ++ej)
                    {
                        if (ej == ei || engaged[static_cast<std::size_t>(ej)] != 0)
                            continue;
                        const auto & rep = m_input.enemies[static_cast<std::size_t>(ej)];
                        if (!rep.canMoveAndFire || rep.hp <= 0.0 || rep.replacementCost <= 0 ||
                            rep.tile == INVALID_TILE)
                            continue;
                        if (indirectProtectedByScreen && !rep.bypassesGroundScreen && rep.maxRange <= 1)
                            continue;
                        if (enemyAttackTurnsToTile(static_cast<std::size_t>(ej), bestSq) > 1)
                            continue;
                        const double myD = static_cast<std::size_t>(ej) < tmpl.baseDamageVsEnemy.size()
                            ? tmpl.baseDamageVsEnemy[static_cast<std::size_t>(ej)] : -1.0;
                        const double retD = static_cast<std::size_t>(ej) < tmpl.returnDamageFromEnemy.size()
                            ? tmpl.returnDamageFromEnemy[static_cast<std::size_t>(ej)] : 0.0;
                        if (firstStrikeNetTrade(tmpl, rep, myD, retD, tmpl.minRange) > 0.0)
                            continue;
                        const double repStrike = static_cast<std::size_t>(ej) < tmpl.enemyFirstStrikeVsMe.size()
                            ? tmpl.enemyFirstStrikeVsMe[static_cast<std::size_t>(ej)] : 0.0;
                        if (repStrike <= 0.0)
                            continue;
                        const double hitHp = std::min(
                            hp, (repStrike * (rep.hp / UNIFIED_PROD_FULL_HP)) / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                        if (hitHp > worstHardHit)
                            worstHardHit = hitHp;
                    }
                    if (worstHardHit > 0.0)
                        hp = std::max(0.0, hp - worstHardHit);
                };
                bool coverCounterApplied = false;
                // Track lowest enemy HP reached so between-turn property repair cannot create phantom farming value.
                double minEnemyHpReached = simEnemyHp;
                double remainingCapHp = simEnemyHp;
                while (simTurn < horizon && simHp > 0.0 && simEnemyHp > 0.0 && simAmmo > 0)
                {
                    if (openFieldMobile && simTurn > fireTurn)
                        break;
                    const double scaledDmg =
                        baseDmg * (simHp / UNIFIED_PROD_FULL_HP) * defenderTerrainFactor(enemy, simEnemyHp);
                    const double dealtHp = std::min(simEnemyHp, scaledDmg / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                    if (dealtHp <= 0.0)
                        break;
                    const double afterEnemyHp = std::max(0.0, simEnemyHp - dealtHp);
                    const double netProgressHp = std::max(0.0, minEnemyHpReached - afterEnemyHp);
                    minEnemyHpReached = std::min(minEnemyHpReached, afterEnemyHp);
                    const double creditedCapHp = std::min(remainingCapHp, dealtHp);
                    remainingCapHp = std::max(0.0, remainingCapHp - creditedCapHp);
                    const double enemyCapLoss =
                        (static_cast<double>(enemy.replacementCost) * creditedCapHp) / UNIFIED_PROD_FULL_HP;
                    const double remThreatStream =
                        (cov < 1.0 && simTurn > 0 && static_cast<std::size_t>(simTurn) < threatFromTurn.size())
                        ? static_cast<double>(threatFromTurn[static_cast<std::size_t>(simTurn)]) * remaining
                        : 0.0;
                    const double denialSaved = remThreatStream * (netProgressHp / UNIFIED_PROD_FULL_HP);
                    double ourCounterLoss = 0.0;
                    double retHp = 0.0;
                    if (tmpl.canMoveAndFire && afterEnemyHp > 0.0 && retDmg > 0.0)
                    {
                        const double scaledRet = retDmg * (afterEnemyHp / UNIFIED_PROD_FULL_HP);
                        retHp = std::min(simHp, scaledRet / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                        ourCounterLoss = (static_cast<double>(tmpl.cost) * retHp) / UNIFIED_PROD_FULL_HP;
                    }
                    else if (!tmpl.canMoveAndFire && afterEnemyHp > 0.0 && enemy.canMoveAndFire &&
                             !(indirectProtectedByScreen && !enemy.bypassesGroundScreen && enemy.maxRange <= 1) &&
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
                    if (simEnemyHp > 0.0 && enemy.repairsOnTile)
                    {
                        simEnemyHp = std::min(
                            UNIFIED_PROD_FULL_HP, simEnemyHp + static_cast<double>(UNIFIED_PROD_REPAIR_STEPS));
                    }
                    // If a non-countering target survives Shot 1, a covering mobile hard-counter strips HP before Shot 2.
                    if (firstTarget && !coverCounterApplied && retDmg <= 0.0 && simEnemyHp > 0.0)
                    {
                        coverCounterApplied = true;
                        applyCoveringHardCounter(simHp);
                    }
                    ++simTurn;
                    --simAmmo;
                }
                // After Target 1, a covering mobile hard-counter strips HP before chaining to Target 2.
                if (firstTarget && !coverCounterApplied && targetGain > 0.0)
                {
                    coverCounterApplied = true;
                    applyCoveringHardCounter(simHp);
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
                        ? std::max(0.0, std::min(remaining, (startEnemyHp - minEnemyHpReached) / enemy.hp))
                        : 0.0;
                    bestEndTurn = simTurn;
                    bestFireTurn = fireTurn;
                    bestAmmoSpent = remAmmo - simAmmo;
                    bestEngageSq = bestSq;
                    bestOpenFieldMobile = openFieldMobile;
                    bestEi = ei;
                }
            }
            if (bestEi < 0 || bestGain <= 0.0)
                break;
            if (firstTarget)
            {
                firstFireTurn = bestFireTurn;
                firstEngageSq = bestEngageSq != INVALID_TILE ? bestEngageSq
                                                             : m_input.enemies[static_cast<std::size_t>(bestEi)].tile;
            }
            firstTarget = false;
            ++targetsEngaged;
            engaged[static_cast<std::size_t>(bestEi)] = 1;
            if (bestClaimedFrac > 0.0)
                claimedCoverage.emplace_back(static_cast<std::size_t>(bestEi), bestClaimedFrac);
            totalOffense += bestGain;
            curTurn = bestEndTurn;
            curHp = bestAfterHp;
            remAmmo -= bestAmmoSpent;
            curTile = bestEngageSq != INVALID_TILE ? bestEngageSq
                                                   : m_input.enemies[static_cast<std::size_t>(bestEi)].tile;
            if (bestOpenFieldMobile && !tmpl.canMoveAndFire && !indirectProtectedByScreen)
                break;
        }

        // Screened or terrain-protected indirects deny contested unowned/enemy properties and protect threatened own sites (no double-count).
        if (!tmpl.canMoveAndFire && tmpl.maxRange > 1 &&
            (!m_input.properties.empty() || !m_input.ownStrategicLoss.empty()))
        {
            const std::size_t stKey =
                static_cast<std::size_t>(siteIdx) * m_input.templates.size() + static_cast<std::size_t>(tmplIdx);
            if (m_fast && stKey < m_fast->indirectZoneTargets.size())
            {
                double bestZoneDenial = 0.0;
                std::int32_t bestZoneTurn = UNIFIED_PROD_UNREACHABLE_TURNS;
                for (const auto & zt : m_fast->indirectZoneTargets[stKey])
                {
                    bool occupiedByEngaged = false;
                    for (const std::size_t occEi : zt.occupyingEnemies)
                    {
                        if (occEi < engaged.size() && engaged[occEi] != 0)
                        {
                            occupiedByEngaged = true;
                            break;
                        }
                    }
                    if (occupiedByEngaged)
                        continue;
                    const std::int32_t bestCost =
                        indirectProtectedByScreen ? zt.bestCostScreened : zt.bestCostUnscreened;
                    if (bestCost == std::numeric_limits<std::int32_t>::max())
                        continue;
                    const std::int32_t moveT = bestCost == 0 ? 0 : (bestCost + tmpl.movementPoints - 1) / tmpl.movementPoints;
                    const std::int32_t zoneTurn = spawnTurn + 1 + moveT + 1;
                    if (zoneTurn >= horizon)
                        continue;
                    const double remFrac = static_cast<double>(horizon - zoneTurn) / static_cast<double>(horizon);
                    const double denial = (zt.fullHorizonStake * remFrac) / static_cast<double>(1 + friendlyIndirectCount);
                    if (denial > bestZoneDenial)
                    {
                        bestZoneDenial = denial;
                        bestZoneTurn = zoneTurn;
                    }
                }
                if (bestZoneDenial > 0.0)
                {
                    totalOffense += bestZoneDenial;
                    firstFireTurn = std::min(firstFireTurn, bestZoneTurn);
                }
            }
        }

        if (firstFireTurn >= UNIFIED_PROD_UNREACHABLE_TURNS)
            firstFireTurn = spawnTurn + 1 + fallbackMoveTurns + (tmpl.canMoveAndFire ? 0 : 1);
        if (totalOffense <= 0.0)
        {
            CombatEvalResult zeroRes{0, firstFireTurn, {}};
            if (m_fast)
                m_fast->combatCache.emplace(std::move(combatKey), zeroRes);
            return zeroRes;
        }

        // Counter-exposure against reachable in-horizon enemies and connected enemy production sites.
        double maxCounterRisk = 0.0;
        for (std::size_t ei = 0; ei < m_input.enemies.size(); ++ei)
        {
            const auto & enemy = m_input.enemies[ei];
            if (indirectProtectedByScreen && !enemy.bypassesGroundScreen && enemy.maxRange <= 1)
                continue;
            if (enemy.hp <= 0.0 || enemy.replacementCost <= 0)
                continue;
            const bool hasEnemyPos = enemy.tile != INVALID_TILE;
            const std::int32_t eReachTurns = hasEnemyPos
                ? ((m_fast && siteTmplEnemyBase + ei < m_fast->siteToEnemyMoveTurns.size())
                       ? m_fast->siteToEnemyMoveTurns[siteTmplEnemyBase + ei]
                       : moveTurnsSiteToEnemy(siteIdx, tmpl, ei))
                : 1;
            if (hasEnemyPos && (eReachTurns >= UNIFIED_PROD_UNREACHABLE_TURNS || spawnTurn + 1 + eReachTurns >= horizon))
                continue;
            const double baseDmg = ei < tmpl.baseDamageVsEnemy.size() ? tmpl.baseDamageVsEnemy[ei] : -1.0;
            const double retDmg = ei < tmpl.returnDamageFromEnemy.size() ? tmpl.returnDamageFromEnemy[ei] : 0.0;
            const double enemyStrike = ei < tmpl.enemyFirstStrikeVsMe.size() ? tmpl.enemyFirstStrikeVsMe[ei] : 0.0;
            const double ourCounter = ei < tmpl.counterDamageWhenStruck.size() ? tmpl.counterDamageWhenStruck[ei] : 0.0;
            const std::int32_t enemyTurnsToUs = hasEnemyPos
                ? enemyAttackTurnsToTile(ei, m_input.sites[static_cast<std::size_t>(siteIdx)].tile)
                : UNIFIED_PROD_UNREACHABLE_TURNS;
            const std::int32_t enemyTurnsToEngage = (hasEnemyPos && firstEngageSq != INVALID_TILE)
                ? enemyAttackTurnsToTile(ei, firstEngageSq)
                : enemyTurnsToUs;
            const double preNetTrade = firstStrikeNetTrade(tmpl, enemy, baseDmg, retDmg, tmpl.minRange);
            // Flank-local cover: any mobile enemy within 1 turn (or a hard counter within 2 turns) of our engagement tile counters us.
            const bool coversEngagement = hasEnemyPos && enemy.canMoveAndFire &&
                (enemyTurnsToEngage <= 1 || (preNetTrade <= 0.0 && enemyTurnsToEngage <= 2));
            if (hasEnemyPos && tmpl.canMoveAndFire && enemy.canMoveAndFire &&
                enemyTurnsToUs > eReachTurns && !coversEngagement)
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
                    counterThreat = std::max(0.0, threat - std::max(0.0, netTrade));
            }
            maxCounterRisk = std::max(maxCounterRisk, counterThreat);
        }
        const std::size_t fcKey =
            static_cast<std::size_t>(siteIdx) * m_input.templates.size() + static_cast<std::size_t>(tmplIdx);
        if (m_fast && fcKey < m_fast->factoryCounters.size())
        {
            for (const auto & fc : m_fast->factoryCounters[fcKey])
            {
                if (spawnTurn + 1 + fc.sReachTurns >= horizon)
                    continue;
                if (indirectProtectedByScreen && fc.skippedWhenScreened)
                    continue;
                if (fc.airAaThreshold >= 0 && friendlyAntiAirCapital >= fc.airAaThreshold)
                    continue;
                if (fc.rawExcess > maxCounterRisk)
                    maxCounterRisk = fc.rawExcess;
            }
        }

        const double net = std::max(0.0, totalOffense - maxCounterRisk);
        CombatEvalResult finalRes{static_cast<MilliFunds>(std::llround(net)), firstFireTurn, std::move(claimedCoverage)};
        if (m_fast)
            m_fast->combatCache.emplace(std::move(combatKey), finalRes);
        return finalRes;
    }

    void UnifiedProductionModel::applyClaimedCoverage(int tmplIdx, std::span<const std::pair<std::size_t, double>> claimed,
                          std::vector<double> & coverage) const
    {
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

    UnifiedProductionModel::CandidateScore UnifiedProductionModel::scoreSiteTemplate(int siteIdx, int tmplIdx, std::int32_t spawnTurn,
                                 std::span<const double> coverage,
                                 std::vector<std::int32_t> & claimedTurns,
                                 MilliFunds friendlyDirectCapital,
                                 std::int32_t friendlyDirectVehicles,
                                 std::int32_t friendlyCombatCount,
                                 ScreenInventory screens,
                                 std::int32_t friendlyIndirectCount,
                                 std::int32_t friendlyCapperCount, bool commitClaims,
                                 MilliFunds friendlyAntiAirCapital) const
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
        out.survivingCapital = tmpl.cost;
        std::vector<std::int32_t> captureClaims;
        if (tmpl.canCapture)
        {
            if (m_fast)
            {
                CapperCacheKey cKey{siteIdx, tmplIdx, spawnTurn, hasCombatPresence, claimedTurns};
                if (const auto it = m_fast->capperCache.find(cKey); it != m_fast->capperCache.end())
                {
                    out.capperGain = it->second.gain;
                    if (commitClaims)
                        captureClaims = it->second.outClaims;
                }
                else
                {
                    captureClaims = claimedTurns;
                    out.capperGain = evaluateCapperGain(
                        siteIdx, tmplIdx, spawnTurn, hasCombatPresence, friendlyCapperCount, captureClaims);
                    m_fast->capperCache.emplace(std::move(cKey), CapperCacheVal{out.capperGain, captureClaims});
                }
            }
            else
            {
                captureClaims = claimedTurns;
                out.capperGain = evaluateCapperGain(
                    siteIdx, tmplIdx, spawnTurn, hasCombatPresence, friendlyCapperCount, captureClaims);
            }
        }
        CombatEvalResult combatRes = evaluateCombatGain(
            siteIdx, tmplIdx, spawnTurn, coverage, friendlyDirectCapital, friendlyDirectVehicles,
            friendlyCombatCount, screens.additionalBodies, friendlyIndirectCount, effAntiAirCap);
        out.combatGain = combatRes.gain;
        out.arrivalTurns = combatRes.firstFireTurn;
        out.claimedCoverage = std::move(combatRes.claimedCoverage);
        out.screenGain = evaluateScreenGain(
            siteIdx, tmplIdx, spawnTurn, friendlyDirectCapital, friendlyDirectVehicles,
            friendlyIndirectCount, screens.additionalBodies + screens.assignedVehicles);
        // Conserve single-unit action economy: each unit performs one primary role (capture, combat, or screen).
        out.utility = std::max({out.capperGain, out.combatGain, out.screenGain});

        const auto & site = m_input.sites[static_cast<std::size_t>(siteIdx)];
        if (spawnTurn == 0 && site.owner == OwnerSign::Ours &&
            (!site.enemyCanStrikeInE0.empty() || !site.enemyCapperCanStepInE0.empty()))
        {
            std::int32_t ownSiteCount = m_fast ? m_fast->ownSiteCount : 0;
            if (!m_fast)
            {
                for (const auto & s : m_input.sites)
                    if (s.owner == OwnerSign::Ours) ++ownSiteCount;
            }
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
            out.survivingCapital = tmpl.cost - ourSpawnLoss;
            if (remHp <= 0.0)
            {
                out.survivingCapital = 0;
                out.screenGain = 0;
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
        // Only the selected surviving role reserves the opportunities it was paid for.
        const MilliFunds roleValue = std::max({out.capperGain, out.combatGain, out.screenGain});
        if (roleValue > 0 && out.survivingCapital > 0)
        {
            if (out.capperGain == roleValue)
            {
                out.role = ProductionRole::Capture;
                if (commitClaims)
                    claimedTurns = std::move(captureClaims);
            }
            else if (out.combatGain == roleValue)
                out.role = ProductionRole::Combat;
            else
                out.role = ProductionRole::Screen;
        }
        return out;
    }

    std::vector<int> UnifiedProductionModel::paretoTemplatesForSite(int siteIdx, MilliFunds budget, std::int32_t spawnTurn,
                                        std::span<const double> coverage,
                                        std::vector<std::int32_t> claimedTurns,
                                        MilliFunds friendlyDirectCapital,
                                        std::int32_t friendlyDirectVehicles,
                                        std::int32_t friendlyCombatCount,
                                        ScreenInventory screens,
                                        std::int32_t friendlyIndirectCount,
                                        std::int32_t friendlyCapperCount,
                                        MilliFunds friendlyAntiAirCapital) const
    {
        const auto & site = m_input.sites[static_cast<std::size_t>(siteIdx)];
        std::vector<CandidateScore> scored;
        scored.reserve(site.buildableTemplateIndices.size());
        for (const int ti : site.buildableTemplateIndices)
        {
            if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                continue;
            if (m_input.templates[static_cast<std::size_t>(ti)].cost > budget)
                continue;
            const auto sc = scoreSiteTemplate(
                siteIdx, ti, spawnTurn, coverage, claimedTurns, friendlyDirectCapital,
                friendlyDirectVehicles, friendlyCombatCount, screens,
                friendlyIndirectCount, friendlyCapperCount, false, friendlyAntiAirCapital);
            if (sc.utility > 0)
                scored.push_back(sc);
        }
        const auto combined = FastCache::selectParetoFrontier(scored, m_input.templates);
        std::vector<int> ids;
        ids.reserve(combined.size());
        for (const auto & sc : combined)
            ids.push_back(sc.templateIndex);
        return ids;
    }

    void UnifiedProductionModel::advancePurchase(GreedyTurnOutcome & state, const CandidateScore & sc) const
    {
        state.remainingFunds -= sc.cost;
        state.utility += sc.utility;
        if (sc.survivingCapital <= 0)
            return;
        const auto & tmpl = m_input.templates[sc.templateIndex];
        if (sc.role == ProductionRole::Combat)
            applyClaimedCoverage(sc.templateIndex, sc.claimedCoverage, state.coverage);
        if (sc.role == ProductionRole::Screen)
        {
            if (tmpl.hasWeapons && tmpl.canMoveAndFire && !tmpl.canCapture)
                ++state.screens.assignedVehicles;
            else
                ++state.screens.additionalBodies;
        }
        if (tmpl.canCapture)
            ++state.capperCount;
        if (tmpl.hasWeapons)
        {
            ++state.combatCount;
            if (templateAntiAirCapable(sc.templateIndex))
                state.antiAirCapital += sc.survivingCapital;
            if (tmpl.canMoveAndFire && !tmpl.canCapture)
            {
                state.directCapital += sc.survivingCapital;
                ++state.directVehicles;
            }
            else if (!tmpl.canMoveAndFire && tmpl.maxRange > 1)
                ++state.indirectCount;
        }
    }

    UnifiedProductionModel::CandidateScore UnifiedProductionModel::quotePurchase(int site, int ti, int wave, GreedyTurnOutcome & state, bool commit) const
    {
        return scoreSiteTemplate(site, ti, wave, state.coverage, state.claims, state.directCapital,
                                 state.directVehicles, state.combatCount, state.screens,
                                 state.indirectCount, state.capperCount, commit, state.antiAirCapital);
    }

    [[gnu::noinline]] UnifiedProductionModel::GreedyTurnOutcome UnifiedProductionModel::evaluateSingleTurnGreedy(std::span<const int> siteIndices, MilliFunds budget,
                                           std::int32_t spawnTurn, std::vector<double> coverage,
                                           std::vector<std::int32_t> claimedTurns,
                                           MilliFunds friendlyDirectCapital,
                                           std::int32_t friendlyDirectVehicles,
                                           std::int32_t friendlyCombatCount,
                                           ScreenInventory screens,
                                           std::int32_t friendlyIndirectCount,
                                           std::int32_t friendlyCapperCount,
                                           MilliFunds friendlyAntiAirCapital) const
    {
        const MilliFunds initAntiAir =
            friendlyAntiAirCapital >= 0 ? friendlyAntiAirCapital : m_input.friendlyAntiAirCapital;
        GreedyTurnOutcome emptyOut{0, std::max<MilliFunds>(0, budget), coverage, claimedTurns,
                                   friendlyDirectCapital, friendlyDirectVehicles, friendlyCombatCount,
                                   screens, friendlyIndirectCount, friendlyCapperCount, initAntiAir};
        if (siteIndices.empty() || budget <= 0)
            return emptyOut;
        // Pre-score initial state once across all sites/templates so both greedy packs and Pareto seeds reuse it.
        const std::size_t nSites = siteIndices.size();
        const std::size_t nTmpls = m_input.templates.size();
        std::vector<std::vector<CandidateScore>> initScores(nSites);
        std::vector<std::int32_t> scratchClaims = claimedTurns;
        for (std::size_t s = 0; s < nSites; ++s)
        {
            const int siteIdx = siteIndices[s];
            const auto & buildables = m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices;
            initScores[s].reserve(buildables.size());
            for (const int ti : buildables)
            {
                if (ti < 0 || static_cast<std::size_t>(ti) >= nTmpls)
                    continue;
                if (m_input.templates[static_cast<std::size_t>(ti)].cost > budget)
                    continue;
                const auto sc = scoreSiteTemplate(
                    siteIdx, ti, spawnTurn, coverage, scratchClaims, friendlyDirectCapital,
                    friendlyDirectVehicles, friendlyCombatCount, screens,
                    friendlyIndirectCount, friendlyCapperCount, false, initAntiAir);
                if (sc.utility > 0)
                    initScores[s].push_back(sc);
            }
        }
        const auto paretoFromScored = [&](const std::vector<CandidateScore> & scored) -> std::vector<int> {
            const auto combined = FastCache::selectParetoFrontier(scored, m_input.templates);
            std::vector<int> ids;
            ids.reserve(combined.size());
            for (const auto & sc : combined) ids.push_back(sc.templateIndex);
            return ids;
        };

        // Compare greedy efficiency pack and raw-utility pack, plus single highest-value unit + greedy fill.
        int topSite = -1;
        int topTmpl = -1;
        const auto runGreedy = [&](int forcedSite, int forcedTmpl, bool packByRawUtility,
                                   bool recordFirstPick = false) -> GreedyTurnOutcome {
            GreedyTurnOutcome out;
            out.remainingFunds = budget;
            out.coverage = coverage;
            out.claims = claimedTurns;
            out.directCapital = friendlyDirectCapital;
            out.directVehicles = friendlyDirectVehicles;
            out.combatCount = friendlyCombatCount;
            out.screens = screens;
            out.indirectCount = friendlyIndirectCount;
            out.capperCount = friendlyCapperCount;
            out.antiAirCapital = initAntiAir;
            std::vector<std::uint8_t> used(siteIndices.size(), 0);
            if (forcedSite >= 0 && forcedTmpl >= 0)
            {
                const CandidateScore * cachedForced = nullptr;
                for (const auto & cand : initScores[static_cast<std::size_t>(forcedSite)])
                {
                    if (cand.templateIndex == forcedTmpl)
                    {
                        cachedForced = &cand;
                        break;
                    }
                }
                if (cachedForced == nullptr || cachedForced->cost > out.remainingFunds || cachedForced->utility <= 0)
                    return emptyOut;
                if (m_input.templates[static_cast<std::size_t>(forcedTmpl)].canCapture)
                {
                    const auto sc = scoreSiteTemplate(
                        siteIndices[static_cast<std::size_t>(forcedSite)], forcedTmpl, spawnTurn, out.coverage,
                        out.claims, out.directCapital, out.directVehicles, out.combatCount, out.screens,
                        out.indirectCount, out.capperCount, true, out.antiAirCapital);
                    if (sc.cost > out.remainingFunds || sc.utility <= 0)
                        return emptyOut;
                    advancePurchase(out, sc);
                }
                else
                {
                    advancePurchase(out, *cachedForced);
                }
                used[static_cast<std::size_t>(forcedSite)] = 1;
            }
            bool firstPick = true;
            for (;;)
            {
                int bestS = -1;
                CandidateScore bestCand;
                double bestEfficiency = -1.0;
                const bool useInitScores = firstPick && forcedSite < 0;
                for (int s = 0; s < static_cast<int>(siteIndices.size()); ++s)
                {
                    if (used[static_cast<std::size_t>(s)])
                        continue;
                    if (useInitScores)
                    {
                        for (const auto & sc : initScores[static_cast<std::size_t>(s)])
                        {
                            if (sc.cost > out.remainingFunds || sc.utility <= 0)
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
                        continue;
                    }
                    const int siteIdx = siteIndices[static_cast<std::size_t>(s)];
                    for (const int ti : m_input.sites[static_cast<std::size_t>(siteIdx)].buildableTemplateIndices)
                    {
                        if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                            continue;
                        if (m_input.templates[static_cast<std::size_t>(ti)].cost > out.remainingFunds)
                            continue;
                        const auto sc = scoreSiteTemplate(
                            siteIdx, ti, spawnTurn, out.coverage, out.claims, out.directCapital,
                            out.directVehicles, out.combatCount, out.screens, out.indirectCount,
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
                if (recordFirstPick && firstPick)
                {
                    topSite = bestS;
                    topTmpl = bestCand.templateIndex;
                }
                firstPick = false;
                if (m_input.templates[static_cast<std::size_t>(bestCand.templateIndex)].canCapture)
                {
                    const auto committed = scoreSiteTemplate(
                        siteIndices[static_cast<std::size_t>(bestS)], bestCand.templateIndex, spawnTurn, out.coverage,
                        out.claims, out.directCapital, out.directVehicles, out.combatCount, out.screens,
                        out.indirectCount, out.capperCount, true, out.antiAirCapital);
                    advancePurchase(out, committed);
                }
                else
                {
                    advancePurchase(out, bestCand);
                }
                used[static_cast<std::size_t>(bestS)] = 1;
            }
            return out;
        };

        GreedyTurnOutcome best = runGreedy(-1, -1, false);
        GreedyTurnOutcome rawPack = runGreedy(-1, -1, true, true);
        if (rawPack.utility > best.utility)
            best = std::move(rawPack);
        // Seed greedy fill from each Pareto cost tier on the lead site per site type so expensive units never lock out higher-total mixes.
        std::vector<int> seedSites;
        if (topSite >= 0)
            seedSites.push_back(topSite);
        std::vector<QString> seenBuildingIds;
        if (topSite >= 0)
            seenBuildingIds.push_back(m_input.sites[static_cast<std::size_t>(siteIndices[static_cast<std::size_t>(topSite)])].buildingId);
        for (int s = 0; s < static_cast<int>(siteIndices.size()); ++s)
        {
            const QString & bId = m_input.sites[static_cast<std::size_t>(siteIndices[static_cast<std::size_t>(s)])].buildingId;
            if (std::ranges::find(seenBuildingIds, bId) == seenBuildingIds.end())
            {
                seenBuildingIds.push_back(bId);
                seedSites.push_back(s);
            }
        }
        for (const int seedS : seedSites)
        {
            std::vector<int> seedTemplates = paretoFromScored(initScores[static_cast<std::size_t>(seedS)]);
            if (seedS == topSite && topTmpl >= 0 &&
                std::ranges::find(seedTemplates, topTmpl) == seedTemplates.end())
            {
                seedTemplates.push_back(topTmpl);
            }
            for (const int ti : seedTemplates)
            {
                GreedyTurnOutcome alt = runGreedy(seedS, ti, false);
                if (alt.utility > best.utility)
                    best = std::move(alt);
            }
        }
        return best;
    }

    MilliFunds UnifiedProductionModel::evaluateTomorrowBest(std::span<const int> allOwnSites, MilliFunds tomorrowFunds,
                                const std::vector<double> & coverage,
                                const std::vector<std::int32_t> & claimedTurns,
                                MilliFunds friendlyDirectCapital,
                                std::int32_t friendlyDirectVehicles,
                                std::int32_t friendlyCombatCount,
                                ScreenInventory screens,
                                std::int32_t friendlyIndirectCount,
                                std::int32_t friendlyCapperCount,
                                MilliFunds friendlyAntiAirCapital) const
    {
        if (allOwnSites.empty() || tomorrowFunds <= 0 || m_input.horizonTurns <= 1)
            return 0;
        const MilliFunds initAntiAir =
            friendlyAntiAirCapital >= 0 ? friendlyAntiAirCapital : m_input.friendlyAntiAirCapital;
        TomorrowCacheKey cacheKey;
        cacheKey.siteCount = static_cast<std::int32_t>(allOwnSites.size());
        cacheKey.firstSite = allOwnSites.front();
        cacheKey.tomorrowFunds = tomorrowFunds;
        cacheKey.directCapital = friendlyDirectCapital;
        cacheKey.directVehicles = friendlyDirectVehicles;
        cacheKey.combatCount =
            (friendlyDirectCapital > 0 || friendlyCombatCount > 0 || m_input.totalFriendlyCombatCapital > 0) ? 1 : 0;
        cacheKey.screenBodies = screens.additionalBodies;
        cacheKey.screenVehicles = screens.assignedVehicles;
        cacheKey.indirectCount = friendlyIndirectCount;
        cacheKey.capperCount = 0;
        cacheKey.antiAirCapital = initAntiAir;
        cacheKey.quantizedCoverage.reserve(coverage.size());
        for (const double c : coverage)
            cacheKey.quantizedCoverage.push_back(static_cast<std::int32_t>(std::llround(c * UNIFIED_PROD_COVERAGE_CACHE_SCALE)));
        cacheKey.claims = claimedTurns;
        if (m_fast)
        {
            if (const auto it = m_fast->tomorrowCache.find(cacheKey); it != m_fast->tomorrowCache.end())
                return it->second;
        }
        else if (const auto it = m_tomorrowCache.find(cacheKey); it != m_tomorrowCache.end())
        {
            return it->second;
        }

        // Roll multi-turn continuation across waves 1..UNIFIED_PROD_FUTURE_WAVES so carried cash retains true future value.
        MilliFunds totalFuture = 0;
        MilliFunds curRem = tomorrowFunds;
        std::vector<double> curCov = coverage;
        std::vector<std::int32_t> curClaims = claimedTurns;
        MilliFunds curDirect = friendlyDirectCapital;
        std::int32_t curDirectVehicles = friendlyDirectVehicles;
        std::int32_t curCombat = friendlyCombatCount;
        auto curScreen = screens;
        std::int32_t curIndirect = friendlyIndirectCount;
        std::int32_t curCapper = friendlyCapperCount;
        MilliFunds curAntiAir = initAntiAir;
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
            curScreen = out.screens;
            curIndirect = out.indirectCount;
            curCapper = out.capperCount;
            curAntiAir = out.antiAirCapital;
        }
        if (m_fast)
            m_fast->tomorrowCache.emplace(std::move(cacheKey), totalFuture);
        else
            m_tomorrowCache.emplace(std::move(cacheKey), totalFuture);
        return totalFuture;
    }

    UnifiedProductionModel::GreedyTurnOutcome UnifiedProductionModel::initialProductionState(MilliFunds funds, const std::vector<double> & coverage,
                                        const std::vector<std::int32_t> & claims) const
    {
        return {0, funds, coverage, claims, m_input.friendlyDirectCapital, effectiveDirectVehicles(),
                m_input.friendlyCombatUnitCount, initialScreens(), m_input.friendlyIndirectCount,
                m_input.friendlyCapperCount, m_input.friendlyAntiAirCapital};
    }

    MilliFunds UnifiedProductionModel::tomorrowAfter(std::span<const int> sites, const GreedyTurnOutcome & state) const
    {
        return evaluateTomorrowBest(sites,
            std::max<MilliFunds>(0, state.remainingFunds + m_input.nextTurnIncome - m_input.nextTurnRepairBill),
            state.coverage, state.claims, state.directCapital, state.directVehicles, state.combatCount,
            state.screens, state.indirectCount, state.capperCount, state.antiAirCapital);
    }

    ProductionBuildOrder UnifiedProductionModel::purchaseOrder(int site, const CandidateScore & sc) const
    {
        return {.tile = m_input.sites[site].tile, .unitId = m_input.templates[sc.templateIndex].unitId,
                .templateIndex = sc.templateIndex, .cost = sc.cost, .survivingCapital = sc.survivingCapital, .utility = sc.utility,
                .capperGain = sc.capperGain, .combatGain = sc.combatGain, .arrivalTurns = sc.arrivalTurns};
    }

    bool UnifiedProductionModel::affordableSearchFits(std::span<const int> sites, MilliFunds funds) const
    {
        std::map<MilliFunds, std::size_t> counts{{0, 1}};
        std::size_t total = 1;
        for (const int site : sites)
        {
            auto next = counts;
            for (const int ti : m_input.sites[site].buildableTemplateIndices)
            {
                if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                    continue;
                const MilliFunds cost = m_input.templates[ti].cost;
                if (cost <= 0 || cost > funds)
                    continue;
                for (const auto & [spent, ways] : counts)
                {
                    if (spent > funds - cost)
                        break;
                    total += ways;
                    if (total > UNIFIED_PROD_EXHAUSTIVE_PLAN_LIMIT)
                        return false;
                    next[spent + cost] += ways;
                }
            }
            counts = std::move(next);
        }
        return true;
    }

    void UnifiedProductionModel::searchExactToday(std::span<const int> sites, std::span<const int> futureSites,
                      MilliFunds funds, const std::vector<double> & coverage,
                      const std::vector<std::int32_t> & claims, ProductionPlan & best, bool exhaustive) const
    {
        std::vector<ProductionBuildOrder> orders;
        const auto visit = [&](auto & self, std::size_t pos, const GreedyTurnOutcome & state) -> void {
            pumpUiEventsIfDue();
            if (pos == sites.size())
            {
                if (orders.empty()) return; // The common SAVE baseline already priced this leaf.
                const MilliFunds tomorrow = tomorrowAfter(futureSites, state);
                const MilliFunds total = state.utility + tomorrow;
                if (std::tie(total, state.utility) > std::tie(best.totalValue, best.todayUtility))
                {
                    best.orders = orders;
                    best.totalValue = total;
                    best.todayUtility = state.utility;
                    best.tomorrowUtility = tomorrow;
                    best.remainingFunds = state.remainingFunds;
                    best.spentFunds = funds - state.remainingFunds;
                }
                return;
            }
            self(self, pos + 1, state);
            const int site = sites[pos];
            if (exhaustive)
            {
                for (const int ti : m_input.sites[site].buildableTemplateIndices)
                {
                    if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()) ||
                        m_input.templates[ti].cost <= 0 || m_input.templates[ti].cost > state.remainingFunds)
                        continue;
                    auto next = state;
                    const auto sc = quotePurchase(site, ti, 0, next, true);
                    if (sc.utility <= 0)
                        continue;
                    advancePurchase(next, sc);
                    orders.push_back(purchaseOrder(site, sc));
                    self(self, pos + 1, next);
                    orders.pop_back();
                }
            }
            else
            {
                // Score once per site/template and reuse non-capper CandidateScore instead of re-scoring on commit.
                const auto & siteInfo = m_input.sites[static_cast<std::size_t>(site)];
                std::vector<CandidateScore> scored;
                scored.reserve(siteInfo.buildableTemplateIndices.size());
                std::vector<std::int32_t> scratchClaims = state.claims;
                for (const int ti : siteInfo.buildableTemplateIndices)
                {
                    if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()))
                        continue;
                    if (m_input.templates[static_cast<std::size_t>(ti)].cost > state.remainingFunds)
                        continue;
                    const auto sc = scoreSiteTemplate(
                        site, ti, 0, state.coverage, scratchClaims, state.directCapital,
                        state.directVehicles, state.combatCount, state.screens,
                        state.indirectCount, state.capperCount, false, state.antiAirCapital);
                    if (sc.utility > 0)
                        scored.push_back(sc);
                }
                const auto combined = FastCache::selectParetoFrontier(scored, m_input.templates);
                for (const auto & preSc : combined)
                {
                    const int ti = preSc.templateIndex;
                    if (ti < 0 || ti >= static_cast<int>(m_input.templates.size()) ||
                        m_input.templates[ti].cost <= 0 || m_input.templates[ti].cost > state.remainingFunds)
                        continue;
                    auto next = state;
                    if (m_input.templates[static_cast<std::size_t>(ti)].canCapture)
                    {
                        const auto sc = quotePurchase(site, ti, 0, next, true);
                        if (sc.utility <= 0) continue;
                        advancePurchase(next, sc);
                        orders.push_back(purchaseOrder(site, sc));
                    }
                    else
                    {
                        advancePurchase(next, preSc);
                        orders.push_back(purchaseOrder(site, preSc));
                    }
                    self(self, pos + 1, next);
                    orders.pop_back();
                }
            }
        };
        visit(visit, 0, initialProductionState(funds, coverage, claims));
    }

    [[gnu::noinline]] void UnifiedProductionModel::searchGreedyWithBigAlternatives(std::span<const int> sites, std::span<const int> futureSites,
                                     MilliFunds funds, const std::vector<double> & coverage,
                                     const std::vector<std::int32_t> & claims, ProductionPlan & best) const
    {
        using Assignment = std::vector<int>;
        using Rank = std::pair<MilliFunds, MilliFunds>; // total, then today's utility (existing tie-break)
        const Assignment empty(sites.size(), -1);
        Assignment bestChoices = empty;
        Rank bestRank{best.totalValue, best.todayUtility};
        std::map<Assignment, Rank> priced{{empty, bestRank}};
        const auto initial = initialProductionState(funds, coverage, claims);
        const auto replay = [&](const Assignment & choices, std::vector<ProductionBuildOrder> * orders = nullptr,
                                bool * hadZeroUtility = nullptr) {
            auto state = initial;
            if (hadZeroUtility) *hadZeroUtility = false;
            for (std::size_t pos = 0; pos < sites.size(); ++pos)
            {
                if (choices[pos] < 0) continue;
                const auto sc = quotePurchase(sites[pos], choices[pos], 0, state, true);
                if (sc.utility <= 0 && hadZeroUtility)
                    *hadZeroUtility = true;
                advancePurchase(state, sc);
                if (orders) orders->push_back(purchaseOrder(sites[pos], sc));
            }
            return state;
        };
        // Every seed and every challenger is priced in the same site order from the unchanged board.
        const auto price = [&](const Assignment & choices) {
            if (const auto it = priced.find(choices); it != priced.end()) return it->second;
            pumpUiEventsIfDue();
            bool hadZero = false;
            const auto state = replay(choices, nullptr, &hadZero);
            if (hadZero)
            {
                const Rank invalid{-1, -1};
                priced.emplace(choices, invalid);
                return invalid;
            }
            const Rank rank{state.utility + tomorrowAfter(futureSites, state), state.utility};
            priced.emplace(choices, rank);
            if (rank > bestRank)
            {
                bestRank = rank;
                bestChoices = choices;
            }
            return rank;
        };
        const auto options = [&](std::size_t pos, MilliFunds budget, const GreedyTurnOutcome & state) {
            return paretoTemplatesForSite(
                sites[pos], budget, 0, state.coverage, state.claims, state.directCapital,
                state.directVehicles, state.combatCount, state.screens, state.indirectCount,
                state.capperCount, state.antiAirCapital);
        };
        struct Addition { std::size_t pos; int ti; double key; MilliFunds utility; };
        const auto pack = [&](Assignment choices, bool byRaw) {
            Rank incumbent = price(choices);
            for (;;)
            {
                auto state = replay(choices);
                std::vector<Addition> additions;
                for (std::size_t pos = 0; pos < sites.size(); ++pos)
                {
                    if (choices[pos] >= 0) continue;
                    for (const int ti : options(pos, state.remainingFunds, state))
                    {
                        const auto sc = quotePurchase(sites[pos], ti, 0, state, false);
                        if (sc.utility <= 0) continue;
                        const double key = byRaw ? static_cast<double>(sc.utility) :
                            static_cast<double>(sc.utility) / static_cast<double>(sc.cost);
                        additions.push_back({pos, ti, key, sc.utility});
                    }
                }
                std::stable_sort(additions.begin(), additions.end(), [](const Addition & a, const Addition & b) {
                    return std::tie(a.key, a.utility) > std::tie(b.key, b.utility);
                });
                bool added = false;
                for (const auto & add : additions)
                {
                    choices[add.pos] = add.ti;
                    const Rank take = price(choices);
                    if (take > incumbent)
                    {
                        incumbent = take;
                        added = true;
                        break;
                    }
                    choices[add.pos] = -1;
                }
                if (!added) return;
            }
        };
        pack(empty, false);
        pack(empty, true);
        for (std::size_t pos = 0; pos < sites.size(); ++pos)
        {
            auto state = initial;
            int top = -1;
            MilliFunds gain = 0;
            for (const int ti : options(pos, funds, state))
            {
                const auto sc = quotePurchase(sites[pos], ti, 0, state, false);
                if (sc.utility > gain) { top = ti; gain = sc.utility; }
            }
            if (top >= 0)
            {
                auto seed = empty;
                seed[pos] = top;
                pack(seed, false);
                pack(seed, true);
            }
        }
        const auto cost = [&](int ti) -> MilliFunds { return ti < 0 ? 0 : m_input.templates[ti].cost; };
        for (int pass = 0; pass < UNIFIED_PROD_GREEDY_SWAP_PASSES; ++pass)
        {
            const Assignment start = bestChoices;
            for (std::size_t pos = 0; pos < sites.size(); ++pos)
            {
                const Assignment base = bestChoices;
                const auto state = replay(base);
                const MilliFunds oldCost = cost(base[pos]);
                auto alternatives = options(pos, state.remainingFunds + oldCost, state);
                alternatives.push_back(-1);
                for (const int ti : alternatives)
                {
                    if (ti == base[pos]) continue;
                    auto trial = base;
                    trial[pos] = ti;
                    price(trial);
                    if (cost(ti) >= oldCost) continue;
                    // Reinvest the released cash at a second site even if downsizing alone loses.
                    const MilliFunds rem = state.remainingFunds + oldCost - cost(ti);
                    for (std::size_t other = 0; other < sites.size(); ++other)
                    {
                        if (other == pos) continue;
                        for (const int replacement : options(other, rem + cost(base[other]), state))
                        {
                            if (replacement == base[other]) continue;
                            trial[other] = replacement;
                            price(trial);
                        }
                        trial[other] = base[other];
                    }
                }
            }
            if (bestChoices == start) break;
        }
        best.orders.clear();
        const auto selected = replay(bestChoices, &best.orders);
        best.totalValue = bestRank.first;
        best.todayUtility = selected.utility;
        best.tomorrowUtility = best.totalValue - best.todayUtility;
        best.remainingFunds = selected.remainingFunds;
        best.spentFunds = funds - selected.remainingFunds;
    }
}
