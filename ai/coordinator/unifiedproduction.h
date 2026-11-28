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
#include <memory>
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
    inline constexpr std::size_t UNIFIED_PROD_EXHAUSTIVE_SITE_LIMIT = 3;
    inline constexpr std::size_t UNIFIED_PROD_EXHAUSTIVE_PLAN_LIMIT = 128;
    inline constexpr std::size_t UNIFIED_PROD_PARETO_PER_SITE = 6;
    inline constexpr int UNIFIED_PROD_GREEDY_SWAP_PASSES = 2;
    inline constexpr double UNIFIED_PROD_COVERAGE_CACHE_SCALE = 1000.0;
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
        std::int32_t defenseStars{0};
        bool repairsOnTile{false};
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
        std::int32_t friendlyVehicleScreens{0};
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
        MilliFunds survivingCapital{0};
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

        explicit UnifiedProductionModel(ProductionModelInput input);

        bool valid() const;

        const ProductionModelInput & input() const { return m_input; }

        MilliFunds currentFunds() const { return m_input.currentFunds; }
        MilliFunds nextTurnIncome() const { return m_input.nextTurnIncome; }

        std::optional<EnemySiteCounterThreat> bestEnemyCounterFromSite(TilePoint enemySite, int friendlyKi) const;

        MilliFunds enemySiteBlockDenial(TilePoint enemySite) const;

        // Exact strategic loss if an enemy captures our own Com Tower, Power Plant, Factory, Airport, or Port.
        MilliFunds ownStrategicCaptureLoss(TilePoint ownTile, std::int32_t horizon) const;

        // Exact strategic denial credit when interrupting an enemy capper on a Com Tower, Power Plant, or production site.
        MilliFunds enemyCaptureInterruptStrategicValue(TilePoint tile, std::int32_t delayTurns,
                                                       std::int32_t horizon) const;

        ProductionPlan planBuilds(std::span<const TilePoint> openFriendlySites,
                                  MilliFunds spendableFunds,
                                  std::span<const std::int32_t> claimedTurnOverride = {}) const;

        // Start the mirrored second wave with exactly the claims and surviving roster of the first.
        void carryPurchasesForward(ProductionModelInput & next, const ProductionPlan & plan) const;

        MilliFunds marginalOpenSitesValue(std::span<const TilePoint> openFriendlySites,
                                          MilliFunds spendableFunds,
                                          std::span<const std::int32_t> claimedTurnOverride = {}) const;

        // Exact horizon utility of placing a free unit of templateIndex at siteTile (evaluated at its true replacementCost).
        MilliFunds zeroCostSpawnHorizonValue(TilePoint siteTile, int templateIndex) const;

        // Exact marginal production surplus from capturing a neutral or enemy Factory, Airport, or Port.
        MilliFunds siteCaptureProductionSurplus(TilePoint siteTile, std::int32_t captureTurn,
                                                std::int32_t horizon) const;

        // Exact combat-bonus capture value for Com Towers and any modded attack/defense buff buildings.
        MilliFunds combatBonusCaptureValue(TilePoint tile, std::int32_t ownTurn, std::int32_t horizon) const;

        // Lists all unowned strategic bonus sites (HQ, Factories, Airports, Ports, Com Towers) for capture valuation.
        std::vector<std::pair<TilePoint, OwnerSign>> strategicCaptureTiles() const;

        static double defenderTerrainFactor(const ProductionEnemyUnit & enemy, double defenderHp);

        static double firstStrikeNetTrade(const ProductionUnitTemplate & unit, const ProductionEnemyUnit & enemy,
                                          double baseDmg, double returnDmg, std::int32_t fireDist = 1);

        static double enemyFirstStrikeNetThreat(const ProductionUnitTemplate & unit, const ProductionEnemyUnit & enemy,
                                                double enemyStrikeDmg, double ourCounterDmg,
                                                std::int32_t fireDist = 1);

    private:
        struct ScreenInventory
        {
            // Vehicles already contribute a physical body through directVehicles.
            std::int32_t additionalBodies{0};
            std::int32_t assignedVehicles{0};
        };

        enum class ProductionRole { None, Capture, Combat, Screen };

        struct CandidateScore
        {
            int templateIndex{-1};
            ProductionRole role{ProductionRole::None};
            MilliFunds survivingCapital{0};
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

        static bool tileEnterable(const MobilityCostGrid & grid, TilePoint t);

        std::vector<std::int32_t> buildReverseDistances(int gridId, std::span<const TilePoint> targets,
                                                        std::int32_t minRange, std::int32_t maxRange) const;

        std::vector<std::int32_t> buildForwardDistances(int gridId, TilePoint origin) const;

        void precompute();

        int findSiteIndex(TilePoint tile, OwnerSign owner) const;

        bool siteBuildsOnlyAir(int siteIdx) const;

        std::int32_t siteMinArrival(int siteIdx) const;

        std::vector<std::int32_t> buildClaimedTurns(std::span<const std::int32_t> overrideTurns) const;

        std::vector<double> buildInitialCoverage() const;

        std::int32_t travelTurnsSiteToProp(int siteIdx, int propIdx, const ProductionUnitTemplate & tmpl) const;

        std::int32_t travelTurnsPropToProp(int pA, int pB, const ProductionUnitTemplate & tmpl) const;

        MilliFunds propertyStreamValue(const ProductionCapturableProperty & prop, std::int32_t ownTurn) const;

        std::int32_t effectiveDirectVehicles() const;

        ScreenInventory initialScreens() const;

        std::int32_t effectiveScreenBodies() const;

        std::int32_t enemyAttackTurnsToTile(std::size_t ei, TilePoint target) const;

        TilePoint bestFiringSquareToEnemy(int siteIdx, bool fromSite, TilePoint fromTile,
                                          const ProductionUnitTemplate & tmpl, std::size_t enemyIdx) const;

        std::int32_t siteToSiteAttackTurns(int fromSiteIdx, int toSiteIdx,
                                            const ProductionUnitTemplate & unitTmpl) const;

        std::int32_t effectiveEnemyDirectCount() const;

        std::int32_t moveTurnsSiteToEnemy(int siteIdx, const ProductionUnitTemplate & tmpl,
                                          std::size_t enemyIdx) const;

        std::int32_t moveTurnsTileToEnemy(TilePoint origin, const ProductionUnitTemplate & tmpl,
                                          std::size_t enemyIdx) const;

        // Traces one newly built capper sequentially over remaining horizon and optionally updates claimedTurns.
        MilliFunds evaluateCapperGain(int siteIdx, int tmplIdx, std::int32_t spawnTurn, bool hasCombatPresence,
                                      std::int32_t friendlyCapperCount,
                                      std::vector<std::int32_t> & claims) const;

        // Exact first-strike body absorption when screening uncovered friendly direct combat units.
        MilliFunds evaluateScreenGain(int siteIdx, int tmplIdx, std::int32_t spawnTurn,
                                      MilliFunds friendlyDirectCapital, std::int32_t friendlyDirectVehicles,
                                      std::int32_t friendlyIndirectCount,
                                      std::int32_t friendlyScreenBodies) const;

        bool allLiveEnemiesCovered(std::span<const double> coverage) const;


        bool templateAntiAirCapable(int tmplIdx) const;

        // Sequential horizon combat trace (exact mirror of evaluateCapperGain): pure conservation, zero weights.
        [[gnu::noinline]] CombatEvalResult evaluateCombatGain(
            int siteIdx, int tmplIdx, std::int32_t spawnTurn, std::span<const double> coverage,
            MilliFunds friendlyDirectCapital, std::int32_t friendlyDirectVehicles,
            std::int32_t friendlyCombatCount, std::int32_t friendlyScreenBodies,
            std::int32_t friendlyIndirectCount, MilliFunds friendlyAntiAirCapital) const;

        void applyClaimedCoverage(int tmplIdx, std::span<const std::pair<std::size_t, double>> claimed,
                                  std::vector<double> & coverage) const;

        CandidateScore scoreSiteTemplate(int siteIdx, int tmplIdx, std::int32_t spawnTurn,
                                         std::span<const double> coverage,
                                         std::vector<std::int32_t> & claimedTurns,
                                         MilliFunds friendlyDirectCapital,
                                         std::int32_t friendlyDirectVehicles,
                                         std::int32_t friendlyCombatCount,
                                         ScreenInventory screens,
                                         std::int32_t friendlyIndirectCount,
                                         std::int32_t friendlyCapperCount, bool commitClaims,
                                         MilliFunds friendlyAntiAirCapital = -1) const;

        std::vector<int> paretoTemplatesForSite(int siteIdx, MilliFunds budget, std::int32_t spawnTurn,
                                                std::span<const double> coverage,
                                                std::vector<std::int32_t> claimedTurns,
                                                MilliFunds friendlyDirectCapital,
                                                std::int32_t friendlyDirectVehicles,
                                                std::int32_t friendlyCombatCount,
                                                ScreenInventory screens,
                                                std::int32_t friendlyIndirectCount,
                                                std::int32_t friendlyCapperCount,
                                                MilliFunds friendlyAntiAirCapital) const;

        struct GreedyTurnOutcome
        {
            MilliFunds utility{0};
            MilliFunds remainingFunds{0};
            std::vector<double> coverage;
            std::vector<std::int32_t> claims;
            MilliFunds directCapital{0};
            std::int32_t directVehicles{0};
            std::int32_t combatCount{0};
            ScreenInventory screens;
            std::int32_t indirectCount{0};
            std::int32_t capperCount{0};
            MilliFunds antiAirCapital{0};
        };

        void advancePurchase(GreedyTurnOutcome & state, const CandidateScore & sc) const;

        CandidateScore quotePurchase(int site, int ti, int wave, GreedyTurnOutcome & state, bool commit) const;

        [[gnu::noinline]] GreedyTurnOutcome evaluateSingleTurnGreedy(std::span<const int> siteIndices, MilliFunds budget,
                                                   std::int32_t spawnTurn, std::vector<double> coverage,
                                                   std::vector<std::int32_t> claimedTurns,
                                                   MilliFunds friendlyDirectCapital,
                                                   std::int32_t friendlyDirectVehicles,
                                                   std::int32_t friendlyCombatCount,
                                                   ScreenInventory screens,
                                                   std::int32_t friendlyIndirectCount,
                                                   std::int32_t friendlyCapperCount,
                                                   MilliFunds friendlyAntiAirCapital = -1) const;

        MilliFunds evaluateTomorrowBest(std::span<const int> allOwnSites, MilliFunds tomorrowFunds,
                                        const std::vector<double> & coverage,
                                        const std::vector<std::int32_t> & claimedTurns,
                                        MilliFunds friendlyDirectCapital,
                                        std::int32_t friendlyDirectVehicles,
                                        std::int32_t friendlyCombatCount,
                                        ScreenInventory screens,
                                        std::int32_t friendlyIndirectCount,
                                        std::int32_t friendlyCapperCount,
                                        MilliFunds friendlyAntiAirCapital = -1) const;

        GreedyTurnOutcome initialProductionState(MilliFunds funds, const std::vector<double> & coverage,
                                                const std::vector<std::int32_t> & claims) const;

        MilliFunds tomorrowAfter(std::span<const int> sites, const GreedyTurnOutcome & state) const;

        ProductionBuildOrder purchaseOrder(int site, const CandidateScore & sc) const;

        // Bound search work by affordable portfolios, including the choice to defer each site.
        bool affordableSearchFits(std::span<const int> sites, MilliFunds funds) const;

        void searchExactToday(std::span<const int> sites, std::span<const int> futureSites,
                              MilliFunds funds, const std::vector<double> & coverage,
                              const std::vector<std::int32_t> & claims, ProductionPlan & best, bool exhaustive) const;

        [[gnu::noinline]] void searchGreedyWithBigAlternatives(std::span<const int> sites, std::span<const int> futureSites,
                                             MilliFunds funds, const std::vector<double> & coverage,
                                             const std::vector<std::int32_t> & claims, ProductionPlan & best) const;

        struct TomorrowCacheKey
        {
            std::int32_t siteCount{0};
            int firstSite{-1};
            MilliFunds tomorrowFunds{0};
            MilliFunds directCapital{0};
            std::int32_t directVehicles{0};
            std::int32_t combatCount{0};
            std::int32_t screenBodies{0};
            std::int32_t screenVehicles{0};
            std::int32_t indirectCount{0};
            std::int32_t capperCount{0};
            MilliFunds antiAirCapital{0};
            std::vector<std::int32_t> quantizedCoverage;
            std::vector<std::int32_t> claims;
            auto operator<=>(const TomorrowCacheKey &) const = default;
        };

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
        mutable std::map<TomorrowCacheKey, MilliFunds> m_tomorrowCache;
        struct FastCache;
        mutable std::shared_ptr<FastCache> m_fast;
    };
}
