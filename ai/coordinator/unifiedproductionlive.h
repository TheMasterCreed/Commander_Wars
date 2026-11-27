#pragma once

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/unifiedproduction.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <vector>

namespace Coordinator
{
    // Builds a live UnifiedProductionModel from the current board for any player count (2p, 3p, 4p, FFA, teams).
    inline UnifiedProductionModel buildLiveUnifiedProductionModel(
        GameMap & map, Player & player, const BattlefieldKnowledge & knowledge,
        MobilityFieldCache & mobilityFields, DamageOracle & oracle,
        std::span<const PropertyFacts> properties, const PropertyStockField & stockField)
    {
        ProductionModelInput input;
        input.width = knowledge.width();
        input.height = knowledge.height();
        input.horizonTurns = stockField.horizonTurns() > 0 ? stockField.horizonTurns() : UNIFIED_PROD_DEFAULT_HORIZON;
        input.capturePointsToCapture =
            stockField.capturePointsToCapture() > 0 ? stockField.capturePointsToCapture() : UNIFIED_PROD_DEFAULT_CAP_POINTS;
        input.currentFunds = toMilliFunds(std::max<qint32>(0, player.getFunds()));
        input.nextTurnIncome = toMilliFunds(std::max<qint32>(0, player.calcIncome()));

        const auto computeForwardGridDist = [&](const MobilityCostGrid & grid, TilePoint origin) {
            const std::size_t cells =
                static_cast<std::size_t>(input.width) * static_cast<std::size_t>(input.height);
            std::vector<std::int32_t> dist(cells, std::numeric_limits<std::int32_t>::max());
            if (!grid.onGrid(origin.x, origin.y))
                return dist;
            const auto idxOf = [&](TilePoint t) {
                return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(input.width) +
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
                    const std::int32_t c = grid.cost(next.x, next.y, dir);
                    if (c == IMPASSABLE_COST)
                        continue;
                    const std::int32_t nd = d + c;
                    if (nd < dist[idxOf(next)])
                    {
                        dist[idxOf(next)] = nd;
                        pq.push({nd, next});
                    }
                }
            }
            return dist;
        };
        const auto lookupGridCost = [&](const std::vector<std::int32_t> & d, TilePoint t) -> std::int32_t {
            if (t.x < 0 || t.y < 0 || t.x >= input.width || t.y >= input.height)
                return std::numeric_limits<std::int32_t>::max();
            const std::size_t idx =
                static_cast<std::size_t>(t.y) * static_cast<std::size_t>(input.width) +
                static_cast<std::size_t>(t.x);
            return idx < d.size() ? d[idx] : std::numeric_limits<std::int32_t>::max();
        };
        struct ActiveLiveCapper
        {
            TilePoint curTile{INVALID_TILE};
            std::int32_t curTurn{0};
            std::int32_t move{1};
            std::int32_t capTurns{2};
            const MobilityCostGrid * grid{nullptr};
            std::vector<std::int32_t> fwd;
            bool fwdDirty{true};
            bool busyInitial{false};
        };
        const auto claimLiveCappersSimultaneously =
            [&](std::int32_t horizon, std::vector<ActiveLiveCapper> & cappers,
                std::vector<ProductionCapturableProperty> & props,
                const std::vector<std::uint8_t> * threatMask, bool hasCombat) -> std::int32_t {
                std::vector<std::uint8_t> claimedByCapper(cappers.size(), 0);
                for (std::size_t ci = 0; ci < cappers.size(); ++ci)
                {
                    if (cappers[ci].busyInitial)
                        claimedByCapper[ci] = 1;
                }
                for (;;)
                {
                    int bestCap = -1;
                    int bestProp = -1;
                    std::int32_t bestOwnTurn = horizon;
                    MilliFunds bestIncome = -1;
                    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
                    for (int ci = 0; ci < static_cast<int>(cappers.size()); ++ci)
                    {
                        auto & c = cappers[static_cast<std::size_t>(ci)];
                        if (c.grid == nullptr || c.curTurn >= horizon - 1)
                            continue;
                        if (c.fwdDirty)
                        {
                            c.fwd = computeForwardGridDist(*c.grid, c.curTile);
                            c.fwdDirty = false;
                        }
                        for (int p = 0; p < static_cast<int>(props.size()); ++p)
                        {
                            const auto & cp = props[static_cast<std::size_t>(p)];
                            if (cp.blocked || cp.claimedTurn < horizon || cp.tile == c.curTile ||
                                !(cp.owner == OwnerSign::Neutral || (hasCombat && cp.owner == OwnerSign::Enemy)))
                                continue;
                            const std::int32_t dist = lookupGridCost(c.fwd, cp.tile);
                            if (dist == std::numeric_limits<std::int32_t>::max())
                                continue;
                            const std::size_t cell =
                                static_cast<std::size_t>(cp.tile.y) * static_cast<std::size_t>(input.width) +
                                static_cast<std::size_t>(cp.tile.x);
                            const bool contested = threatMask != nullptr && cell < threatMask->size() &&
                                (*threatMask)[cell] != 0;
                            const std::int32_t clearTurns = cp.owner == OwnerSign::Enemy ? 1 : (contested ? 1 : 0);
                            const std::int32_t moveTurns = (dist + c.move - 1) / c.move;
                            const std::int32_t arrivalTurn =
                                c.curTurn + std::max<std::int32_t>(1, moveTurns) - 1 + clearTurns;
                            const std::int32_t ownTurn = arrivalTurn + c.capTurns;
                            if (ownTurn >= horizon)
                                continue;
                            if (ownTurn < bestOwnTurn ||
                                (ownTurn == bestOwnTurn &&
                                 (cp.income > bestIncome || (cp.income == bestIncome && dist < bestDist))))
                            {
                                bestOwnTurn = ownTurn;
                                bestIncome = cp.income;
                                bestDist = dist;
                                bestCap = ci;
                                bestProp = p;
                            }
                        }
                    }
                    if (bestCap < 0 || bestProp < 0)
                        break;
                    auto & chosenProp = props[static_cast<std::size_t>(bestProp)];
                    auto & chosenCap = cappers[static_cast<std::size_t>(bestCap)];
                    const std::size_t chosenCell =
                        static_cast<std::size_t>(chosenProp.tile.y) * static_cast<std::size_t>(input.width) +
                        static_cast<std::size_t>(chosenProp.tile.x);
                    const bool chosenContested = threatMask != nullptr && chosenCell < threatMask->size() &&
                        (*threatMask)[chosenCell] != 0;
                    chosenProp.claimedTurn = bestOwnTurn;
                    chosenCap.curTile = chosenProp.tile;
                    chosenCap.curTurn = (chosenProp.owner == OwnerSign::Enemy || chosenContested)
                        ? horizon
                        : bestOwnTurn;
                    chosenCap.fwdDirty = true;
                    claimedByCapper[static_cast<std::size_t>(bestCap)] = 1;
                }
                return static_cast<std::int32_t>(std::count(claimedByCapper.begin(), claimedByCapper.end(), std::uint8_t{1}));
            };

        const QStringList allowedBuildList = player.getBuildList();
        const auto isAllowedForPlayer = [&](const QString & unitId, const QStringList & buildList) {
            return buildList.isEmpty() || buildList.contains(unitId);
        };

        // 1. Collect all visible production sites (Factories, Airports, Ports) and unique buildable unit IDs.
        std::map<QString, int> templateById;
        std::vector<QString> uniqueUnitIds;
        std::map<QString, int> gridByUnitId;

        const auto ensureTemplateIndex = [&](const QString & unitId, TilePoint sampleTile) -> int {
            if (const auto it = templateById.find(unitId); it != templateById.end())
                return it->second;
            const int idx = static_cast<int>(uniqueUnitIds.size());
            templateById.emplace(unitId, idx);
            uniqueUnitIds.push_back(unitId);

            Unit probe(unitId, &player, false, &map);
            ProductionUnitTemplate tmpl;
            tmpl.unitId = unitId;
            const qint32 costFunds = player.getCosts(unitId, QPoint(sampleTile.x, sampleTile.y));
            tmpl.cost = toMilliFunds(std::max<qint32>(0, costFunds > 0 ? costFunds : probe.getUnitCosts()));
            tmpl.movementPoints = probe.getBaseMovementPoints();
            const QPoint pos(sampleTile.x, sampleTile.y);
            tmpl.minRange = std::max<qint32>(1, probe.getMinRange(pos));
            tmpl.maxRange = std::max<qint32>(1, probe.getMaxRange(pos));
            tmpl.maxAmmo = std::max<qint32>(0, probe.getMaxAmmo1());
            tmpl.canMoveAndFire = probe.canMoveAndFire(pos);
            tmpl.canCapture = probe.canCapture();
            tmpl.captureRate = tmpl.canCapture ? std::max<qint32>(0, probe.getCaptureRate(pos)) : 0;
            tmpl.hasWeapons = probe.hasWeapons();
            tmpl.isTransporter = probe.getLoadingPlace() > 0;
            tmpl.unitType = probe.getUnitType();
            tmpl.bypassesGroundDefense = (tmpl.unitType == GameEnums::UnitType_Air);
            if (tmpl.movementPoints > 0 && (tmpl.canCapture || tmpl.hasWeapons))
            {
                const int gridId = static_cast<int>(input.grids.size());
                input.grids.push_back(mobilityFields.grid(map, player, unitId));
                tmpl.gridId = gridId;
                gridByUnitId.emplace(unitId, gridId);
            }
            input.templates.push_back(std::move(tmpl));
            return idx;
        };

        std::vector<int> siteEnemyOwnerId;
        for (const auto & prop : properties)
        {
            pumpUiEventsIfDue();
            if (!prop.canProduce || !map.onMap(prop.x, prop.y))
                continue;
            const Relation rel = knowledge.relation(prop.ownerId);
            if (rel == Relation::Allied)
                continue;
            Building * pBuilding = map.getTerrain(prop.x, prop.y)->getBuilding();
            if (pBuilding == nullptr)
                continue;

            UnifiedProductionSite site;
            site.tile = {prop.x, prop.y};
            site.owner = rel == Relation::Own ? OwnerSign::Ours
                : (rel == Relation::Enemy ? OwnerSign::Enemy : OwnerSign::Neutral);
            site.buildingId = prop.buildingId;
            site.defenseStars = std::max<qint32>(0, map.getTerrain(prop.x, prop.y)->getBaseDefense());
            QStringList enemyAllowedBuildList;
            if (rel == Relation::Enemy && prop.ownerId >= 0 && prop.ownerId < map.getPlayerCount())
            {
                if (Player * pEnemy = map.getPlayer(prop.ownerId))
                {
                    site.ownerAvailableFunds =
                        toMilliFunds(std::max<qint32>(0, pEnemy->getFunds()) + std::max<qint32>(0, pEnemy->calcIncome()));
                    enemyAllowedBuildList = pEnemy->getBuildList();
                }
            }
            const QStringList constructionList = pBuilding->getConstructionList();
            for (const QString & unitId : constructionList)
            {
                if (site.owner == OwnerSign::Enemy)
                {
                    if (!isAllowedForPlayer(unitId, enemyAllowedBuildList))
                        continue;
                }
                else if (!isAllowedForPlayer(unitId, allowedBuildList))
                {
                    continue;
                }
                const int ti = ensureTemplateIndex(unitId, site.tile);
                site.buildableTemplateIndices.push_back(ti);
            }
            siteEnemyOwnerId.push_back(site.owner == OwnerSign::Enemy ? prop.ownerId : NO_OWNER);
            input.sites.push_back(std::move(site));
        }

        // Scale open-ended production horizon to actual MobilityCostGrid factory-to-property capper travel on large maps.
        if (stockField.horizonTurns() <= UNIFIED_PROD_DEFAULT_HORIZON)
        {
            std::int32_t maxCapperTravelTurns = 0;
            for (const auto & site : input.sites)
            {
                if (site.owner != OwnerSign::Ours)
                    continue;
                int capTi = -1;
                for (const int ti : site.buildableTemplateIndices)
                {
                    if (ti >= 0 && ti < static_cast<int>(input.templates.size()) &&
                        input.templates[static_cast<std::size_t>(ti)].canCapture &&
                        input.templates[static_cast<std::size_t>(ti)].movementPoints > 0 &&
                        input.templates[static_cast<std::size_t>(ti)].gridId >= 0)
                    {
                        if (capTi < 0 ||
                            input.templates[static_cast<std::size_t>(ti)].movementPoints >
                                input.templates[static_cast<std::size_t>(capTi)].movementPoints)
                        {
                            capTi = ti;
                        }
                    }
                }
                if (capTi < 0)
                    continue;
                const auto & capTmpl = input.templates[static_cast<std::size_t>(capTi)];
                const auto fwd = computeForwardGridDist(
                    input.grids[static_cast<std::size_t>(capTmpl.gridId)], site.tile);
                for (const auto & prop : properties)
                {
                    if (!prop.capturable || knowledge.relation(prop.ownerId) == Relation::Own ||
                        knowledge.relation(prop.ownerId) == Relation::Allied)
                        continue;
                    const std::int32_t d = lookupGridCost(fwd, {prop.x, prop.y});
                    if (d < std::numeric_limits<std::int32_t>::max())
                    {
                        maxCapperTravelTurns = std::max(
                            maxCapperTravelTurns, (d + capTmpl.movementPoints - 1) / capTmpl.movementPoints);
                    }
                }
            }
            if (maxCapperTravelTurns > 0)
            {
                input.horizonTurns = std::clamp(
                    maxCapperTravelTurns + UNIFIED_PROD_MIN_POST_CAP_TURNS,
                    input.horizonTurns,
                    UNIFIED_PROD_MAX_MAP_HORIZON);
            }
        }

        // 2. Collect live enemy units, friendly combat capital, and expected next-turn auto-repair drain.
        std::map<std::pair<int, QString>, int> enemyGridCache;
        const auto ensureTargetGridIndex = [&](Player & ownerPlayer, const QString & unitId,
                                               ProductionModelInput & targetInp,
                                               std::map<std::pair<int, QString>, int> & gridMap) -> int {
            const std::pair<int, QString> key{ownerPlayer.getPlayerID(), unitId};
            if (const auto it = gridMap.find(key); it != gridMap.end())
                return it->second;
            const int gid = static_cast<int>(targetInp.grids.size());
            targetInp.grids.push_back(mobilityFields.grid(map, ownerPlayer, unitId));
            gridMap.emplace(key, gid);
            return gid;
        };
        std::vector<Unit *> liveEnemyUnits;
        std::vector<std::unique_ptr<Unit>> projectedEnemyProbes;
        std::map<std::int32_t, int> liveEnemyByUnitType;
        std::vector<std::pair<int, Unit *>> liveFriendlyUnits;
        MilliFunds expectedRepairDrain = 0;
        std::int32_t totalMobileCappers = 0;

        for (int ki = 0; ki < static_cast<int>(knowledge.units().size()); ++ki)
        {
            const auto & known = knowledge.units()[static_cast<std::size_t>(ki)];
            if (known.carrier != KnownUnit::NO_CARRIER || known.hpRounded <= 0 || !map.onMap(known.x, known.y))
                continue;
            Unit * pUnit = map.getTerrain(known.x, known.y)->getUnit();
            if (pUnit == nullptr)
                continue;
            const Relation rel = knowledge.relation(known.ownerId);
            if (rel == Relation::Own)
            {
                liveFriendlyUnits.emplace_back(ki, pUnit);
                if (pUnit->getHp() < UNIFIED_PROD_FULL_HP)
                {
                    if (Building * pRepairBldg = map.getTerrain(known.x, known.y)->getBuilding();
                        pRepairBldg != nullptr && pRepairBldg->getOwner() == &player && pRepairBldg->canRepair(pUnit))
                    {
                        const double healHp = std::min(
                            static_cast<double>(UNIFIED_PROD_REPAIR_STEPS),
                            UNIFIED_PROD_FULL_HP - static_cast<double>(pUnit->getHp()));
                        const qint32 uCost = player.getCosts(pUnit->getUnitID(), QPoint(known.x, known.y));
                        const MilliFunds repairCost = toMilliFunds(std::max<qint32>(0, uCost > 0 ? uCost : pUnit->getUnitCosts()));
                        expectedRepairDrain += static_cast<MilliFunds>(std::llround(
                            (static_cast<double>(repairCost) * healHp) / UNIFIED_PROD_FULL_HP));
                    }
                }
                if (known.canCapture)
                {
                    ++input.friendlyCapperCount;
                    if (known.canMoveAndFire && known.movementPoints > 0)
                        ++totalMobileCappers;
                }
                if (known.hasWeapons)
                {
                    const MilliFunds liveCap = static_cast<MilliFunds>(std::llround(
                        static_cast<double>(toMilliFunds(pUnit->getUnitCosts())) *
                        (std::max(0.0, static_cast<double>(pUnit->getHp())) / UNIFIED_PROD_FULL_HP)));
                    input.totalFriendlyCombatCapital += liveCap;
                    ++input.friendlyCombatUnitCount;
                    if (known.canMoveAndFire && !known.canCapture)
                    {
                        input.friendlyDirectCapital += liveCap;
                        ++input.friendlyDirectVehicleCount;
                    }
                    else if (!known.canMoveAndFire && known.maxRange > 1)
                    {
                        ++input.friendlyIndirectCount;
                    }
                }
                continue;
            }
            if (rel != Relation::Enemy)
                continue;
            ProductionEnemyUnit eu;
            eu.knowledgeUnitIndex = ki;
            eu.tile = {known.x, known.y};
            eu.replacementCost = toMilliFunds(std::max<qint32>(0, pUnit->getUnitCosts()));
            eu.hp = std::max(0.0, static_cast<double>(pUnit->getHp()));
            eu.movementPoints = known.movementPoints;
            eu.minRange = std::max<qint32>(1, known.minRange);
            eu.maxRange = std::max<qint32>(1, known.maxRange);
            eu.canMoveAndFire = known.canMoveAndFire;
            eu.canCapture = known.canCapture;
            eu.captureRate = known.canCapture ? std::max<std::int32_t>(0, known.captureRate) : 0;
            eu.unitType = pUnit->getUnitType();
            eu.bypassesGroundScreen = (eu.unitType == GameEnums::UnitType_Air);
            if (pUnit->getOwner() != nullptr && known.movementPoints > 0)
                eu.gridId = ensureTargetGridIndex(*pUnit->getOwner(), known.unitId, input, enemyGridCache);
            if (!eu.bypassesGroundScreen)
            {
                const MobilityCostGrid & eGrid = (eu.gridId >= 0 && eu.gridId < static_cast<int>(input.grids.size()))
                    ? input.grids[static_cast<std::size_t>(eu.gridId)]
                    : mobilityFields.grid(map, *pUnit->getOwner(), known.unitId);
                std::int32_t adjOpen = 0;
                for (const EntryDirection dir : ENTRY_DIRECTIONS)
                {
                    const StepVector step = entryStep(dir);
                    const int ax = eu.tile.x + step.dx;
                    const int ay = eu.tile.y + step.dy;
                    if (!eGrid.onGrid(ax, ay))
                        continue;
                    const bool enterable = std::ranges::any_of(ENTRY_DIRECTIONS, [&](EntryDirection d2) {
                        return eGrid.cost(ax, ay, d2) != IMPASSABLE_COST;
                    });
                    if (enterable)
                        ++adjOpen;
                }
                eu.directAttackSlots = std::max<std::int32_t>(1, adjOpen);
                if (eu.hp > 0.0)
                    input.totalFrontDirectSlots += eu.directAttackSlots;
            }
            if (known.hasWeapons)
            {
                const MilliFunds liveCap = static_cast<MilliFunds>(std::llround(
                    static_cast<double>(eu.replacementCost) * (eu.hp / UNIFIED_PROD_FULL_HP)));
                input.totalEnemyCombatCapital += liveCap;
                if (eu.bypassesGroundScreen)
                    input.enemyAirCapital += liveCap;
                if (known.canMoveAndFire)
                {
                    input.enemyDirectCapital += liveCap;
                    ++input.enemyDirectCount;
                }
                ++liveEnemyByUnitType[eu.unitType];
            }
            input.enemies.push_back(eu);
            liveEnemyUnits.push_back(pUnit);
        }
        input.nextTurnRepairBill = std::min<MilliFunds>(input.currentFunds + input.nextTurnIncome, expectedRepairDrain);

        // 3. Project optimal enemy builds using mirrored Section 13 UnifiedProductionModel (Waves 0 and 1).
        const std::size_t liveEnemyCount = input.enemies.size();
        const bool eHasCo = player.getCO(0) != nullptr || player.getCO(1) != nullptr;
        std::map<QString, int> enemySiteCountByBuilding;
        std::map<std::int32_t, std::vector<const PropertyFacts *>> enemySitesByOwner;
        std::map<std::int32_t, MilliFunds> enemyBaseIncome;
        std::map<std::pair<std::int32_t, std::int32_t>, MilliFunds> enemyBlockDenialByTile;
        for (const auto & prop : properties)
        {
            if (prop.canProduce && knowledge.relation(prop.ownerId) == Relation::Enemy)
            {
                ++enemySiteCountByBuilding[prop.buildingId];
                enemySitesByOwner[prop.ownerId].push_back(&prop);
                if (!enemyBaseIncome.contains(prop.ownerId) &&
                    prop.ownerId >= 0 && prop.ownerId < map.getPlayerCount())
                {
                    if (Player * pEnemy = map.getPlayer(prop.ownerId))
                    {
                        enemyBaseIncome.emplace(
                            prop.ownerId,
                            toMilliFunds(std::max<qint32>(0, pEnemy->calcIncome())));
                    }
                }
            }
        }
        const auto applyTemplateCov = [&](const ProductionUnitTemplate & tmpl,
                                          std::vector<ProductionEnemyUnit> & targets) {
            if (!tmpl.hasWeapons || targets.empty())
                return;
            struct CovCand
            {
                std::size_t ei{0};
                double share{0.0};
                int isAir{0};
                MilliFunds cost{0};
            };
            std::vector<CovCand> cands;
            for (std::size_t ei = 0; ei < targets.size(); ++ei)
            {
                const auto & target = targets[ei];
                const double rem = std::max(0.0, 1.0 - std::max(0.0, target.initialFriendlyCoverage));
                if (rem <= 0.0 || target.hp <= 0.0 || target.replacementCost <= 0)
                    continue;
                const double baseDmg = ei < tmpl.baseDamageVsEnemy.size() ? tmpl.baseDamageVsEnemy[ei] : -1.0;
                const double retDmg = ei < tmpl.returnDamageFromEnemy.size() ? tmpl.returnDamageFromEnemy[ei] : 0.0;
                const double netTrade = UnifiedProductionModel::firstStrikeNetTrade(tmpl, target, baseDmg, retDmg);
                const double liveCap =
                    (static_cast<double>(target.replacementCost) * target.hp) / UNIFIED_PROD_FULL_HP;
                if (netTrade > 0.0 && liveCap > 0.0)
                {
                    const double share = std::min(rem, netTrade / liveCap);
                    if (share > 0.0)
                        cands.push_back({ei, share, target.bypassesGroundScreen ? 1 : 0, target.replacementCost});
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
                targets[c.ei].initialFriendlyCoverage =
                    std::min(1.0, targets[c.ei].initialFriendlyCoverage + alloc);
                budgetLeft -= alloc;
            }
        };
        for (const auto & [enemyOwnerId, ownerSites] : enemySitesByOwner)
        {
            Player * pEnemy = (enemyOwnerId >= 0 && enemyOwnerId < map.getPlayerCount())
                ? map.getPlayer(enemyOwnerId) : nullptr;
            if (pEnemy == nullptr || ownerSites.empty())
                continue;
            MilliFunds eRepairDrain = 0;
            for (std::size_t ei = 0; ei < liveEnemyCount; ++ei)
            {
                Unit * pUnit = liveEnemyUnits[ei];
                const auto & eu = input.enemies[ei];
                if (pUnit == nullptr || pUnit->getOwner() != pEnemy || eu.hp >= UNIFIED_PROD_FULL_HP)
                    continue;
                if (!map.onMap(eu.tile.x, eu.tile.y))
                    continue;
                if (Building * pRb = map.getTerrain(eu.tile.x, eu.tile.y)->getBuilding();
                    pRb != nullptr && pRb->getOwner() == pEnemy && pRb->canRepair(pUnit))
                {
                    const double healHp = std::min(
                        static_cast<double>(UNIFIED_PROD_REPAIR_STEPS),
                        UNIFIED_PROD_FULL_HP - eu.hp);
                    eRepairDrain += static_cast<MilliFunds>(std::llround(
                        (static_cast<double>(eu.replacementCost) * healHp) / UNIFIED_PROD_FULL_HP));
                }
            }
            const MilliFunds eIncome = enemyBaseIncome[enemyOwnerId];
            const MilliFunds eWave0Budget =
                toMilliFunds(std::max<qint32>(0, pEnemy->getFunds())) + enemyBaseIncome[enemyOwnerId];
            if (eWave0Budget <= 0 && eIncome <= 0)
                continue;

            ProductionModelInput eInp;
            eInp.width = input.width;
            eInp.height = input.height;
            eInp.horizonTurns = input.horizonTurns;
            eInp.capturePointsToCapture = input.capturePointsToCapture;
            eInp.currentFunds = eWave0Budget;
            eInp.nextTurnIncome = eIncome;
            eInp.nextTurnRepairBill = std::min(eWave0Budget + eIncome, eRepairDrain);

            const QStringList enemyAllowed = pEnemy->getBuildList();
            std::map<QString, int> eTemplateById;
            std::vector<std::unique_ptr<Unit>> eTemplateProbes;
            const auto ensureEnemyTemplate = [&](const QString & unitId, TilePoint sampleTile) -> int {
                if (const auto it = eTemplateById.find(unitId); it != eTemplateById.end())
                    return it->second;
                const int idx = static_cast<int>(eInp.templates.size());
                eTemplateById.emplace(unitId, idx);
                auto probe = std::make_unique<Unit>(unitId, pEnemy, false, &map);
                ProductionUnitTemplate tmpl;
                tmpl.unitId = unitId;
                const QPoint pos(sampleTile.x, sampleTile.y);
                const qint32 costFunds = pEnemy->getCosts(unitId, pos);
                tmpl.cost = toMilliFunds(std::max<qint32>(0, costFunds > 0 ? costFunds : probe->getUnitCosts()));
                tmpl.movementPoints = probe->getBaseMovementPoints();
                tmpl.minRange = std::max<qint32>(1, probe->getMinRange(pos));
                tmpl.maxRange = std::max<qint32>(1, probe->getMaxRange(pos));
                tmpl.maxAmmo = std::max<qint32>(0, probe->getMaxAmmo1());
                tmpl.canMoveAndFire = probe->canMoveAndFire(pos);
                tmpl.canCapture = probe->canCapture();
                tmpl.captureRate = tmpl.canCapture ? std::max<qint32>(0, probe->getCaptureRate(pos)) : 0;
                tmpl.hasWeapons = probe->hasWeapons();
                tmpl.isTransporter = probe->getLoadingPlace() > 0;
                tmpl.unitType = probe->getUnitType();
                tmpl.bypassesGroundDefense = (tmpl.unitType == GameEnums::UnitType_Air);
                if (tmpl.movementPoints > 0 && (tmpl.canCapture || tmpl.hasWeapons))
                {
                    const int gid = static_cast<int>(eInp.grids.size());
                    eInp.grids.push_back(mobilityFields.grid(map, *pEnemy, unitId));
                    tmpl.gridId = gid;
                }
                eInp.templates.push_back(std::move(tmpl));
                eTemplateProbes.push_back(std::move(probe));
                return idx;
            };

            std::vector<TilePoint> openWave0Tiles;
            std::vector<TilePoint> allEnemyTiles;
            for (const PropertyFacts * pProp : ownerSites)
            {
                const PropertyFacts & prop = *pProp;
                if (!map.onMap(prop.x, prop.y))
                    continue;
                Building * pBuilding = map.getTerrain(prop.x, prop.y)->getBuilding();
                if (pBuilding == nullptr)
                    continue;
                UnifiedProductionSite site;
                site.tile = {prop.x, prop.y};
                site.owner = OwnerSign::Ours;
                site.buildingId = prop.buildingId;
                site.defenseStars = std::max<qint32>(0, map.getTerrain(prop.x, prop.y)->getBaseDefense());
                for (const QString & unitId : pBuilding->getConstructionList())
                {
                    if (!isAllowedForPlayer(unitId, enemyAllowed))
                        continue;
                    site.buildableTemplateIndices.push_back(ensureEnemyTemplate(unitId, site.tile));
                }
                allEnemyTiles.push_back(site.tile);
                if (map.getTerrain(prop.x, prop.y)->getUnit() == nullptr)
                    openWave0Tiles.push_back(site.tile);
                eInp.sites.push_back(std::move(site));
            }
            if (allEnemyTiles.empty())
                continue;
            for (const auto & ourSite : input.sites)
            {
                if (ourSite.owner != OwnerSign::Ours)
                    continue;
                UnifiedProductionSite mirrorSite;
                mirrorSite.tile = ourSite.tile;
                mirrorSite.owner = OwnerSign::Enemy;
                mirrorSite.buildingId = ourSite.buildingId;
                mirrorSite.ownerAvailableFunds = input.currentFunds;
                for (const int ti : ourSite.buildableTemplateIndices)
                {
                    if (ti < 0 || ti >= static_cast<int>(input.templates.size()))
                        continue;
                    mirrorSite.buildableTemplateIndices.push_back(
                        ensureEnemyTemplate(input.templates[static_cast<std::size_t>(ti)].unitId, ourSite.tile));
                }
                eInp.sites.push_back(std::move(mirrorSite));
            }

            // From pEnemy's view, our live units are their targets (eInp.enemies).
            std::map<std::pair<int, QString>, int> eEnemyGridCache;
            std::vector<Unit *> eTargetUnits;
            for (const auto & [ki, pFriendly] : liveFriendlyUnits)
            {
                const auto & fKnown = knowledge.units()[static_cast<std::size_t>(ki)];
                ProductionEnemyUnit targetEu;
                targetEu.knowledgeUnitIndex = ki;
                targetEu.tile = {fKnown.x, fKnown.y};
                const qint32 fCost = player.getCosts(fKnown.unitId, QPoint(fKnown.x, fKnown.y));
                targetEu.replacementCost =
                    toMilliFunds(std::max<qint32>(0, fCost > 0 ? fCost : pFriendly->getUnitCosts()));
                targetEu.hp = std::max(0.0, static_cast<double>(pFriendly->getHp()));
                targetEu.movementPoints = fKnown.movementPoints;
                targetEu.minRange = std::max<qint32>(1, fKnown.minRange);
                targetEu.maxRange = std::max<qint32>(1, fKnown.maxRange);
                targetEu.canMoveAndFire = fKnown.canMoveAndFire;
                targetEu.canCapture = fKnown.canCapture;
                targetEu.captureRate = fKnown.canCapture ? std::max<std::int32_t>(0, fKnown.captureRate) : 0;
                targetEu.unitType = pFriendly->getUnitType();
                targetEu.bypassesGroundScreen = (targetEu.unitType == GameEnums::UnitType_Air);
                if (fKnown.movementPoints > 0)
                    targetEu.gridId = ensureTargetGridIndex(player, fKnown.unitId, eInp, eEnemyGridCache);
                if (!targetEu.bypassesGroundScreen)
                {
                    const MobilityCostGrid & fGrid =
                        (targetEu.gridId >= 0 && targetEu.gridId < static_cast<int>(eInp.grids.size()))
                        ? eInp.grids[static_cast<std::size_t>(targetEu.gridId)]
                        : mobilityFields.grid(map, player, fKnown.unitId);
                    std::int32_t adjOpen = 0;
                    for (const EntryDirection dir : ENTRY_DIRECTIONS)
                    {
                        const StepVector step = entryStep(dir);
                        const int ax = targetEu.tile.x + step.dx;
                        const int ay = targetEu.tile.y + step.dy;
                        if (!fGrid.onGrid(ax, ay))
                            continue;
                        const bool enterable = std::ranges::any_of(ENTRY_DIRECTIONS, [&](EntryDirection d2) {
                            return fGrid.cost(ax, ay, d2) != IMPASSABLE_COST;
                        });
                        if (enterable)
                            ++adjOpen;
                    }
                    targetEu.directAttackSlots = std::max<std::int32_t>(1, adjOpen);
                    if (targetEu.hp > 0.0)
                        eInp.totalFrontDirectSlots += targetEu.directAttackSlots;
                }
                if (fKnown.hasWeapons)
                {
                    const MilliFunds liveCap = static_cast<MilliFunds>(std::llround(
                        static_cast<double>(targetEu.replacementCost) * (targetEu.hp / UNIFIED_PROD_FULL_HP)));
                    eInp.totalEnemyCombatCapital += liveCap;
                    if (fKnown.canMoveAndFire && !fKnown.canCapture)
                    {
                        eInp.enemyDirectCapital += liveCap;
                        ++eInp.enemyDirectCount;
                    }
                }
                eInp.enemies.push_back(targetEu);
                eTargetUnits.push_back(pFriendly);
            }

            // From pEnemy's view, pEnemy's live units provide friendly counts and initial coverage.
            std::vector<ActiveLiveCapper> eLiveCappers;
            std::int32_t eTotalMobileCappers = 0;
            for (std::size_t ei = 0; ei < liveEnemyCount; ++ei)
            {
                Unit * pEUnit = liveEnemyUnits[ei];
                const auto & eu = input.enemies[ei];
                if (pEUnit == nullptr || pEUnit->getOwner() != pEnemy || eu.hp <= 0.0)
                    continue;
                if (eu.canCapture)
                {
                    ++eInp.friendlyCapperCount;
                    if (eu.canMoveAndFire && eu.movementPoints > 0)
                        ++eTotalMobileCappers;
                }
                if (pEUnit->hasWeapons())
                {
                    const MilliFunds liveCap = static_cast<MilliFunds>(std::llround(
                        static_cast<double>(eu.replacementCost) * (eu.hp / UNIFIED_PROD_FULL_HP)));
                    eInp.totalFriendlyCombatCapital += liveCap;
                    ++eInp.friendlyCombatUnitCount;
                    if (eu.canMoveAndFire && !eu.canCapture)
                    {
                        eInp.friendlyDirectCapital += liveCap;
                        ++eInp.friendlyDirectVehicleCount;
                    }
                    else if (!eu.canMoveAndFire && eu.maxRange > 1)
                    {
                        ++eInp.friendlyIndirectCount;
                    }
                }
                if (pEUnit->hasWeapons() && !eInp.enemies.empty())
                {
                    ProductionUnitTemplate eOwnTmpl;
                    eOwnTmpl.cost = eu.replacementCost;
                    eOwnTmpl.canMoveAndFire = eu.canMoveAndFire;
                    eOwnTmpl.minRange = eu.minRange;
                    eOwnTmpl.maxRange = eu.maxRange;
                    eOwnTmpl.hasWeapons = true;
                    eOwnTmpl.baseDamageVsEnemy.assign(eInp.enemies.size(), -1.0);
                    eOwnTmpl.returnDamageFromEnemy.assign(eInp.enemies.size(), 0.0);
                    for (std::size_t ti = 0; ti < eInp.enemies.size(); ++ti)
                    {
                        const double rawDmg = oracle.baseDamageWithAmmo(
                            *pEUnit, *eTargetUnits[ti], eu.minRange, eu.minRange, eu.maxRange);
                        if (rawDmg <= 0.0)
                            continue;
                        eOwnTmpl.baseDamageVsEnemy[ti] = rawDmg * (eu.hp / UNIFIED_PROD_FULL_HP);
                        if (eu.canMoveAndFire && eInp.enemies[ti].canMoveAndFire)
                        {
                            eOwnTmpl.returnDamageFromEnemy[ti] =
                                std::max(0.0, oracle.baseDamageWithAmmo(*eTargetUnits[ti], *pEUnit, 1, 1, 1));
                        }
                    }
                    applyTemplateCov(eOwnTmpl, eInp.enemies);
                }
                if (eu.canCapture && eu.captureRate > 0 && eu.movementPoints > 0)
                {
                    const int gid = ensureEnemyTemplate(pEUnit->getUnitID(), eu.tile);
                    const int gIdx = gid >= 0 && gid < static_cast<int>(eInp.templates.size())
                        ? eInp.templates[static_cast<std::size_t>(gid)].gridId : -1;
                    const MobilityCostGrid * capGrid =
                        (gIdx >= 0 && gIdx < static_cast<int>(eInp.grids.size())) ? &eInp.grids[static_cast<std::size_t>(gIdx)] : nullptr;
                    std::int32_t curTurn = 0;
                    const std::int32_t capTurns = (eInp.capturePointsToCapture + eu.captureRate - 1) / eu.captureRate;
                    if (eu.knowledgeUnitIndex >= 0 && eu.knowledgeUnitIndex < static_cast<int>(knowledge.units().size()) &&
                        knowledge.units()[static_cast<std::size_t>(eu.knowledgeUnitIndex)].capturePoints > 0)
                    {
                        const std::int32_t cPts =
                            knowledge.units()[static_cast<std::size_t>(eu.knowledgeUnitIndex)].capturePoints;
                        curTurn = std::max<std::int32_t>(
                            1, (eInp.capturePointsToCapture - cPts + eu.captureRate - 1) / eu.captureRate);
                    }
                    if (eu.hp > 0.0 && curTurn < eInp.horizonTurns - 1 && capGrid != nullptr)
                    {
                        eLiveCappers.push_back({eu.tile, curTurn, std::max<int>(1, eu.movementPoints), capTurns, capGrid, {}, true});
                    }
                }
            }

            const std::size_t eTmplCount = eInp.templates.size();
            const std::size_t eTargetCount = eInp.enemies.size();
            for (std::size_t ti = 0; ti < eTmplCount; ++ti)
            {
                auto & tmpl = eInp.templates[ti];
                tmpl.baseDamageVsEnemy.assign(eTargetCount, -1.0);
                tmpl.returnDamageFromEnemy.assign(eTargetCount, 0.0);
                tmpl.enemyFirstStrikeVsMe.assign(eTargetCount, 0.0);
                tmpl.counterDamageWhenStruck.assign(eTargetCount, 0.0);
                tmpl.baseDamageVsTemplate.assign(eTmplCount, -1.0);
                Unit & probe = *eTemplateProbes[ti];
                if (tmpl.hasWeapons)
                {
                    for (std::size_t tj = 0; tj < eTmplCount; ++tj)
                    {
                        tmpl.baseDamageVsTemplate[tj] = oracle.baseDamageWithAmmo(
                            probe, *eTemplateProbes[tj], tmpl.minRange, tmpl.minRange, tmpl.maxRange);
                    }
                }
                for (std::size_t ei = 0; ei < eTargetCount; ++ei)
                {
                    Unit * pTarget = eTargetUnits[ei];
                    const auto & target = eInp.enemies[ei];
                    if (tmpl.hasWeapons)
                    {
                        tmpl.baseDamageVsEnemy[ei] = oracle.baseDamageWithAmmo(
                            probe, *pTarget, tmpl.minRange, tmpl.minRange, tmpl.maxRange);
                        if (tmpl.canMoveAndFire && target.canMoveAndFire)
                        {
                            tmpl.returnDamageFromEnemy[ei] =
                                std::max(0.0, oracle.baseDamageWithAmmo(*pTarget, probe, 1, 1, 1));
                        }
                    }
                    const double eStrike = oracle.baseDamageWithAmmo(
                        *pTarget, probe, target.minRange, target.minRange, target.maxRange);
                    if (eStrike > 0.0)
                    {
                        tmpl.enemyFirstStrikeVsMe[ei] = eStrike;
                        if (target.canMoveAndFire && tmpl.canMoveAndFire && tmpl.hasWeapons)
                        {
                            tmpl.counterDamageWhenStruck[ei] =
                                std::max(0.0, oracle.baseDamageWithAmmo(probe, *pTarget, 1, 1, 1));
                        }
                    }
                }
            }

            const MilliFunds eFutureScale = eIncome * std::max<std::int32_t>(1, eInp.horizonTurns / 2);
            const MilliFunds eCombinedArmies =
                eInp.totalFriendlyCombatCapital + eInp.totalEnemyCombatCapital + eFutureScale * 2;
            std::map<QString, int> eOwnSame;
            for (const auto & s : eInp.sites)
                if (s.owner == OwnerSign::Ours) ++eOwnSame[s.buildingId];
            for (const auto & prop : properties)
            {
                if (!prop.capturable)
                    continue;
                const Relation rel = knowledge.relation(prop.ownerId);
                if (rel == Relation::Enemy)
                {
                    if (prop.ownerId == enemyOwnerId && map.onMap(prop.x, prop.y))
                    {
                        if (Building * pB = map.getTerrain(prop.x, prop.y)->getBuilding())
                        {
                            MilliFunds ownLoss = 0;
                            const std::int32_t bonusPct = std::max<qint32>(0, pB->getOffensiveBonus()) +
                                std::max<qint32>(0, pB->getDefensiveBonus()) +
                                (eHasCo ? std::max<qint32>(0, pB->getPowerChargeBonus()) / 2 : 0);
                            if (bonusPct > 0)
                            {
                                ownLoss += static_cast<MilliFunds>(std::llround(
                                    static_cast<double>(eCombinedArmies) *
                                    (static_cast<double>(bonusPct) / UNIFIED_PROD_PERCENT_SCALE)));
                            }
                            if (prop.canProduce && eIncome > 0)
                                ownLoss += eIncome / std::max(1, eOwnSame[prop.buildingId]);
                            if (ownLoss > 0)
                                eInp.ownStrategicLoss.emplace(std::pair{prop.x, prop.y}, ownLoss);
                        }
                    }
                    continue;
                }
                if (rel == Relation::Allied)
                    continue;
                ProductionCapturableProperty cp;
                cp.tile = {prop.x, prop.y};
                cp.income = toMilliFunds(std::max<qint32>(0, prop.incomePerTurn > 0 ? prop.incomePerTurn : prop.baseIncome));
                cp.owner = rel == Relation::Own ? OwnerSign::Enemy : OwnerSign::Neutral;
                cp.claimedTurn = eInp.horizonTurns;
                cp.blocked = false;
                if (map.onMap(prop.x, prop.y))
                {
                    if (Building * pB = map.getTerrain(prop.x, prop.y)->getBuilding())
                    {
                        cp.offensiveBonus = std::max<qint32>(0, pB->getOffensiveBonus());
                        cp.defensiveBonus = std::max<qint32>(0, pB->getDefensiveBonus());
                        if (eHasCo)
                            cp.powerChargeBonus = std::max<qint32>(0, pB->getPowerChargeBonus());
                    }
                }
                eInp.properties.push_back(cp);
            }
            const bool eHasCombat = eInp.totalFriendlyCombatCapital > 0 || eInp.friendlyDirectCapital > 0;
            const std::int32_t eBusy0 = claimLiveCappersSimultaneously(
                eInp.horizonTurns, eLiveCappers, eInp.properties, nullptr, eHasCombat);
            eInp.friendlyScreenBodies = std::max<std::int32_t>(0, eTotalMobileCappers - eBusy0);

            const auto appendProjectedEnemy = [&](const QString & unitId, TilePoint spawnTile,
                                                  MilliFunds cost, std::int32_t delayWave) {
                auto probe = std::make_unique<Unit>(unitId, pEnemy, false, &map);
                const QPoint pos(spawnTile.x, spawnTile.y);
                ProductionEnemyUnit eu;
                eu.knowledgeUnitIndex = NO_UNIT;
                eu.tile = spawnTile;
                eu.replacementCost = cost;
                eu.hp = UNIFIED_PROD_FULL_HP;
                eu.movementPoints = probe->getBaseMovementPoints();
                eu.minRange = std::max<qint32>(1, probe->getMinRange(pos));
                eu.maxRange = std::max<qint32>(1, probe->getMaxRange(pos));
                eu.canMoveAndFire = probe->canMoveAndFire(pos);
                eu.canCapture = probe->canCapture();
                eu.captureRate = eu.canCapture ? std::max<std::int32_t>(0, probe->getCaptureRate(pos)) : 0;
                eu.unitType = probe->getUnitType();
                eu.spawnDelayTurns = delayWave;
                eu.bypassesGroundScreen = (eu.unitType == GameEnums::UnitType_Air);
                eu.isProjected = true;
                if (eu.movementPoints > 0)
                    eu.gridId = ensureTargetGridIndex(*pEnemy, unitId, input, enemyGridCache);
                if (delayWave == 0 && eu.canMoveAndFire && !eu.canCapture)
                {
                    input.enemyDirectCapital += cost;
                    ++input.enemyDirectCount;
                }
                if (delayWave == 0 && eu.bypassesGroundScreen && probe->hasWeapons())
                    input.enemyAirCapital += cost;
                if (probe->hasWeapons())
                    ++liveEnemyByUnitType[eu.unitType];
                input.enemies.push_back(eu);
                liveEnemyUnits.push_back(probe.get());
                projectedEnemyProbes.push_back(std::move(probe));
            };

            UnifiedProductionModel eModel0(eInp);
            const ProductionPlan ePlan0 = openWave0Tiles.empty()
                ? ProductionPlan{.remainingFunds = eWave0Budget}
                : eModel0.planBuilds(openWave0Tiles, eWave0Budget);
            for (const auto & ord : ePlan0.orders)
                appendProjectedEnemy(ord.unitId, ord.tile, ord.cost, 0);

            ProductionModelInput eInp1 = std::move(eInp);
            const MilliFunds eWave1Budget = std::max<MilliFunds>(0, ePlan0.remainingFunds + eIncome - eInp1.nextTurnRepairBill);
            eInp1.currentFunds = eWave1Budget;
            eInp1.nextTurnRepairBill = 0;
            std::int32_t eTotalMobileCappers1 = eTotalMobileCappers;
            for (const auto & ord : ePlan0.orders)
            {
                if (ord.templateIndex < 0 || ord.templateIndex >= static_cast<int>(eInp1.templates.size()))
                    continue;
                const auto & ot = eInp1.templates[static_cast<std::size_t>(ord.templateIndex)];
                if (ot.hasWeapons)
                {
                    ++eInp1.friendlyCombatUnitCount;
                    eInp1.totalFriendlyCombatCapital += ord.cost;
                    if (ot.canMoveAndFire && !ot.canCapture)
                    {
                        eInp1.friendlyDirectCapital += ord.cost;
                        ++eInp1.friendlyDirectVehicleCount;
                    }
                    else if (!ot.canMoveAndFire && ot.maxRange > 1)
                    {
                        ++eInp1.friendlyIndirectCount;
                    }
                    applyTemplateCov(ot, eInp1.enemies);
                }
                if (ot.canCapture)
                {
                    ++eInp1.friendlyCapperCount;
                    if (ot.canMoveAndFire && ot.movementPoints > 0)
                        ++eTotalMobileCappers1;
                    if (ot.gridId >= 0 && ot.gridId < static_cast<int>(eInp1.grids.size()) && ot.captureRate > 0)
                    {
                        const std::int32_t cTurns = (eInp1.capturePointsToCapture + ot.captureRate - 1) / ot.captureRate;
                        eLiveCappers.push_back({ord.tile, 1, std::max<int>(1, ot.movementPoints), cTurns,
                                                &eInp1.grids[static_cast<std::size_t>(ot.gridId)], {}, true, false});
                    }
                }
            }
            const bool eHasCombat1 = eInp1.totalFriendlyCombatCapital > 0 || eInp1.friendlyDirectCapital > 0;
            const std::int32_t eBusy1 = claimLiveCappersSimultaneously(
                eInp1.horizonTurns, eLiveCappers, eInp1.properties, nullptr, eHasCombat1);
            eInp1.friendlyScreenBodies = std::max<std::int32_t>(0, eTotalMobileCappers1 - eBusy1);
            std::optional<UnifiedProductionModel> eModel1;
            ProductionPlan ePlan1;
            if (eWave1Budget > 0 && eInp1.horizonTurns > 1)
            {
                eModel1.emplace(eInp1);
                ePlan1 = eModel1->planBuilds(allEnemyTiles, eWave1Budget);
                for (const auto & ord : ePlan1.orders)
                    appendProjectedEnemy(ord.unitId, ord.tile, ord.cost, 1);
            }
            const std::int32_t blockHorizon = std::max<std::int32_t>(1, input.horizonTurns);
            for (const TilePoint sTile : allEnemyTiles)
            {
                MilliFunds marginal = 0;
                if (std::ranges::find(openWave0Tiles, sTile) != openWave0Tiles.end())
                {
                    std::vector<TilePoint> withoutTile;
                    withoutTile.reserve(openWave0Tiles.size());
                    for (const TilePoint t : openWave0Tiles)
                    {
                        if (t != sTile)
                            withoutTile.push_back(t);
                    }
                    const MilliFunds withoutVal = withoutTile.empty()
                        ? ePlan0.saveOnlyValue : eModel0.planBuilds(withoutTile, eWave0Budget).totalValue;
                    marginal = std::max<MilliFunds>(0, ePlan0.totalValue - withoutVal);
                }
                else if (eModel1.has_value())
                {
                    std::vector<TilePoint> withoutTile;
                    withoutTile.reserve(allEnemyTiles.size());
                    for (const TilePoint t : allEnemyTiles)
                    {
                        if (t != sTile)
                            withoutTile.push_back(t);
                    }
                    const MilliFunds withoutVal = withoutTile.empty()
                        ? ePlan1.saveOnlyValue : eModel1->planBuilds(withoutTile, eWave1Budget).totalValue;
                    marginal = std::max<MilliFunds>(0, ePlan1.totalValue - withoutVal);
                }
                const MilliFunds perTurnBlock = marginal / blockHorizon;
                if (perTurnBlock > 0)
                    enemyBlockDenialByTile.emplace(std::pair{sTile.x, sTile.y}, perTurnBlock);
            }
        }

        // 4. Compute exact existing friendly coverage on each enemy unit, allocating specialized responders first.
        std::vector<Unit *> airProbeCheck;
        std::vector<std::unique_ptr<Unit>> ownedAirProbes;
        for (const auto & tmpl : input.templates)
        {
            if (tmpl.bypassesGroundDefense && tmpl.hasWeapons)
            {
                ownedAirProbes.push_back(std::make_unique<Unit>(tmpl.unitId, &player, false, &map));
                airProbeCheck.push_back(ownedAirProbes.back().get());
            }
        }
        struct LiveFriendlyPlan
        {
            std::size_t orderIdx{0};
            std::size_t targetCount{0};
        };
        std::vector<LiveFriendlyPlan> friendlyOrder;
        friendlyOrder.reserve(liveFriendlyUnits.size());
        for (std::size_t fi = 0; fi < liveFriendlyUnits.size(); ++fi)
        {
            const auto & [ki, pFriendly] = liveFriendlyUnits[fi];
            const auto & known = knowledge.units()[static_cast<std::size_t>(ki)];
            const double fHp = std::max(0.0, static_cast<double>(pFriendly->getHp()));
            if (known.hasWeapons && fHp > 0.0)
            {
                const qint32 minR = std::max<qint32>(1, known.minRange);
                const qint32 maxR = std::max<qint32>(1, known.maxRange);
                const bool countersAir = std::ranges::any_of(airProbeCheck, [&](Unit * pAir) {
                    const double d = oracle.baseDamageWithAmmo(*pFriendly, *pAir, minR, minR, maxR);
                    return (d / UNIFIED_PROD_HP_PERCENT_PER_STEP) * 2.0 >= UNIFIED_PROD_FULL_HP;
                });
                if (countersAir)
                {
                    const MilliFunds fCost = toMilliFunds(std::max<qint32>(0, pFriendly->getUnitCosts()));
                    input.friendlyAntiAirCapital += static_cast<MilliFunds>(
                        std::llround(static_cast<double>(fCost) * (fHp / UNIFIED_PROD_FULL_HP)));
                }
            }
            std::size_t posTargets = 0;
            if (known.hasWeapons && fHp > 0.0)
            {
                ProductionUnitTemplate fTmpl;
                fTmpl.cost = toMilliFunds(std::max<qint32>(0, pFriendly->getUnitCosts()));
                fTmpl.canMoveAndFire = known.canMoveAndFire;
                fTmpl.minRange = std::max<qint32>(1, known.minRange);
                fTmpl.maxRange = std::max<qint32>(1, known.maxRange);
                for (std::size_t ei = 0; ei < input.enemies.size(); ++ei)
                {
                    Unit * pEnemy = liveEnemyUnits[ei];
                    const auto & enemy = input.enemies[ei];
                    if (pEnemy == nullptr || enemy.hp <= 0.0 || enemy.replacementCost <= 0)
                        continue;
                    const double rawDmg = oracle.baseDamageWithAmmo(
                        *pFriendly, *pEnemy, fTmpl.minRange, fTmpl.minRange, fTmpl.maxRange);
                    if (rawDmg <= 0.0)
                        continue;
                    const double retDmg = (fTmpl.canMoveAndFire && enemy.canMoveAndFire)
                        ? std::max(0.0, oracle.baseDamageWithAmmo(*pEnemy, *pFriendly, 1, 1, 1))
                        : 0.0;
                    if (UnifiedProductionModel::firstStrikeNetTrade(
                            fTmpl, enemy, rawDmg * (fHp / UNIFIED_PROD_FULL_HP), retDmg) > 0.0)
                    {
                        ++posTargets;
                    }
                }
            }
            friendlyOrder.push_back({fi, posTargets});
        }
        std::stable_sort(friendlyOrder.begin(), friendlyOrder.end(), [](const LiveFriendlyPlan & a, const LiveFriendlyPlan & b) {
            return std::tuple{a.targetCount, a.orderIdx} < std::tuple{b.targetCount, b.orderIdx};
        });
        for (const auto & fpPlan : friendlyOrder)
        {
            const auto & [ki, pFriendly] = liveFriendlyUnits[fpPlan.orderIdx];
            pumpUiEventsIfDue();
            const auto & known = knowledge.units()[static_cast<std::size_t>(ki)];
            if (!known.hasWeapons)
                continue;
            const double fHp = std::max(0.0, static_cast<double>(pFriendly->getHp()));
            if (fHp <= 0.0)
                continue;
            const MilliFunds fCost = toMilliFunds(std::max<qint32>(0, pFriendly->getUnitCosts()));
            ProductionUnitTemplate fTmpl;
            fTmpl.cost = fCost;
            fTmpl.canMoveAndFire = known.canMoveAndFire;
            fTmpl.minRange = std::max<qint32>(1, known.minRange);
            fTmpl.maxRange = std::max<qint32>(1, known.maxRange);
            struct LiveCovCand
            {
                std::size_t ei{0};
                double share{0.0};
                int isAir{0};
                MilliFunds cost{0};
            };
            std::vector<LiveCovCand> contribs;
            for (std::size_t ei = 0; ei < input.enemies.size(); ++ei)
            {
                const double rem = std::max(0.0, 1.0 - std::max(0.0, input.enemies[ei].initialFriendlyCoverage));
                if (rem <= 0.0)
                    continue;
                Unit * pEnemy = liveEnemyUnits[ei];
                const auto & enemy = input.enemies[ei];
                if (pEnemy == nullptr || enemy.hp <= 0.0 || enemy.replacementCost <= 0)
                    continue;
                const double rawDmg = oracle.baseDamageWithAmmo(
                    *pFriendly, *pEnemy, fTmpl.minRange, fTmpl.minRange, fTmpl.maxRange);
                if (rawDmg <= 0.0)
                    continue;
                const double scaledDmg = rawDmg * (fHp / UNIFIED_PROD_FULL_HP);
                const double retDmg = (fTmpl.canMoveAndFire && enemy.canMoveAndFire)
                    ? std::max(0.0, oracle.baseDamageWithAmmo(*pEnemy, *pFriendly, 1, 1, 1))
                    : 0.0;
                const double trade = UnifiedProductionModel::firstStrikeNetTrade(fTmpl, enemy, scaledDmg, retDmg);
                const double liveCap = (static_cast<double>(enemy.replacementCost) * enemy.hp) / UNIFIED_PROD_FULL_HP;
                if (trade > 0.0 && liveCap > 0.0)
                {
                    const double share = std::min(rem, trade / liveCap);
                    if (share > 0.0)
                        contribs.push_back({ei, share, enemy.bypassesGroundScreen ? 1 : 0, enemy.replacementCost});
                }
            }
            if (!contribs.empty())
            {
                std::stable_sort(contribs.begin(), contribs.end(), [](const LiveCovCand & a, const LiveCovCand & b) {
                    return std::tuple{a.isAir, a.cost, a.share} > std::tuple{b.isAir, b.cost, b.share};
                });
                double budgetLeft = 1.0;
                for (const auto & c : contribs)
                {
                    if (budgetLeft <= 0.0)
                        break;
                    const double alloc = std::min(budgetLeft, c.share);
                    input.enemies[c.ei].initialFriendlyCoverage =
                        std::min(1.0, input.enemies[c.ei].initialFriendlyCoverage + alloc);
                    budgetLeft -= alloc;
                }
            }
        }

        // 5. Fill exact pairwise damage tables for every buildable template against every enemy unit and template.
        const std::size_t enemyCount = input.enemies.size();
        const std::size_t tmplCount = input.templates.size();
        std::vector<std::unique_ptr<Unit>> templateProbes;
        templateProbes.reserve(tmplCount);
        for (const auto & tmpl : input.templates)
            templateProbes.push_back(std::make_unique<Unit>(tmpl.unitId, &player, false, &map));
        for (std::size_t ti = 0; ti < tmplCount; ++ti)
        {
            pumpUiEventsIfDue();
            auto & tmpl = input.templates[ti];
            tmpl.baseDamageVsEnemy.assign(enemyCount, -1.0);
            tmpl.returnDamageFromEnemy.assign(enemyCount, 0.0);
            tmpl.enemyFirstStrikeVsMe.assign(enemyCount, 0.0);
            tmpl.counterDamageWhenStruck.assign(enemyCount, 0.0);
            tmpl.baseDamageVsTemplate.assign(tmplCount, -1.0);
            Unit & probe = *templateProbes[ti];
            if (tmpl.hasWeapons)
            {
                for (std::size_t tj = 0; tj < tmplCount; ++tj)
                {
                    tmpl.baseDamageVsTemplate[tj] = oracle.baseDamageWithAmmo(
                        probe, *templateProbes[tj], tmpl.minRange, tmpl.minRange, tmpl.maxRange);
                }
            }
            if (!tmpl.hasWeapons && enemyCount == 0)
                continue;
            for (std::size_t ei = 0; ei < enemyCount; ++ei)
            {
                Unit * pEnemy = liveEnemyUnits[ei];
                const auto & enemy = input.enemies[ei];
                if (pEnemy == nullptr)
                    continue;
                if (tmpl.hasWeapons)
                {
                    const double dmg = oracle.baseDamageWithAmmo(
                        probe, *pEnemy, tmpl.minRange, tmpl.minRange, tmpl.maxRange);
                    tmpl.baseDamageVsEnemy[ei] = dmg;
                    if (tmpl.canMoveAndFire && enemy.canMoveAndFire)
                    {
                        tmpl.returnDamageFromEnemy[ei] =
                            std::max(0.0, oracle.baseDamageWithAmmo(*pEnemy, probe, 1, 1, 1));
                    }
                }
                const double eStrike = oracle.baseDamageWithAmmo(
                    *pEnemy, probe, enemy.minRange, enemy.minRange, enemy.maxRange);
                if (eStrike > 0.0)
                {
                    tmpl.enemyFirstStrikeVsMe[ei] = eStrike;
                    if (enemy.canMoveAndFire && tmpl.canMoveAndFire && tmpl.hasWeapons)
                    {
                        tmpl.counterDamageWhenStruck[ei] =
                            std::max(0.0, oracle.baseDamageWithAmmo(probe, *pEnemy, 1, 1, 1));
                    }
                }
            }
        }

        // 6. Populate exact enemy-site counter threats (replaces UNIFIED_FACTORY_COUNTER_* placeholders).
        std::map<std::pair<std::int32_t, QString>, int> enemyGridByKey;
        const auto ensureEnemyGridIndex = [&](Player & enemyPlayer, const QString & unitId) -> int {
            const auto key = std::pair{enemyPlayer.getPlayerID(), unitId};
            if (const auto it = enemyGridByKey.find(key); it != enemyGridByKey.end())
                return it->second;
            const int gid = static_cast<int>(input.grids.size());
            input.grids.push_back(mobilityFields.grid(map, enemyPlayer, unitId));
            enemyGridByKey.emplace(key, gid);
            return gid;
        };
        for (const auto & prop : properties)
        {
            pumpUiEventsIfDue();
            if (!prop.canProduce || knowledge.relation(prop.ownerId) != Relation::Enemy ||
                !map.onMap(prop.x, prop.y))
                continue;
            Building * pBuilding = map.getTerrain(prop.x, prop.y)->getBuilding();
            Player * pEnemy = (prop.ownerId >= 0 && prop.ownerId < map.getPlayerCount()) ? map.getPlayer(prop.ownerId) : nullptr;
            if (pBuilding == nullptr || pEnemy == nullptr)
                continue;
            const MilliFunds enemyBudget =
                toMilliFunds(std::max<qint32>(0, pEnemy->getFunds()) + std::max<qint32>(0, pEnemy->calcIncome()));
            const QStringList enemyAllowed = pEnemy->getBuildList();

            struct EnemySpawnProbe
            {
                std::unique_ptr<Unit> unit;
                int gridId{-1};
                MilliFunds cost{0};
                std::int32_t move{0};
                std::int32_t minR{1};
                std::int32_t maxR{1};
                bool canMoveAndFire{true};
            };
            std::vector<EnemySpawnProbe> spawns;
            MilliFunds maxAffordableCost = 0;
            const QPoint pos(prop.x, prop.y);
            for (const QString & unitId : pBuilding->getConstructionList())
            {
                if (!isAllowedForPlayer(unitId, enemyAllowed))
                    continue;
                auto probe = std::make_unique<Unit>(unitId, pEnemy, false, &map);
                const qint32 costFunds = pEnemy->getCosts(unitId, pos);
                const MilliFunds cost = toMilliFunds(std::max<qint32>(0, costFunds > 0 ? costFunds : probe->getUnitCosts()));
                if (cost <= 0 || cost > enemyBudget)
                    continue;
                if (cost > maxAffordableCost)
                    maxAffordableCost = cost;
                if (!probe->hasWeapons())
                    continue;
                const std::int32_t move = probe->getBaseMovementPoints();
                const std::int32_t minR = std::max<qint32>(1, probe->getMinRange(pos));
                const std::int32_t maxR = std::max<qint32>(1, probe->getMaxRange(pos));
                const bool canMoveAndFire = probe->canMoveAndFire(pos);
                const int spGridId = move > 0 ? ensureEnemyGridIndex(*pEnemy, unitId) : -1;
                spawns.push_back(EnemySpawnProbe{
                    .unit = std::move(probe),
                    .gridId = spGridId,
                    .cost = cost,
                    .move = move,
                    .minR = minR,
                    .maxR = maxR,
                    .canMoveAndFire = canMoveAndFire,
                });
            }
            const TilePoint siteTile{prop.x, prop.y};
            if (const auto bIt = enemyBlockDenialByTile.find(std::pair{siteTile.x, siteTile.y});
                bIt != enemyBlockDenialByTile.end() && bIt->second > 0)
            {
                input.enemySiteBlockValue.emplace(std::pair{siteTile.x, siteTile.y}, bIt->second);
            }
            for (const auto & [ki, pFriendly] : liveFriendlyUnits)
            {
                const auto & known = knowledge.units()[static_cast<std::size_t>(ki)];
                const qint32 fCostRaw = player.getCosts(known.unitId, QPoint(known.x, known.y));
                const MilliFunds fCost =
                    toMilliFunds(std::max<qint32>(0, fCostRaw > 0 ? fCostRaw : pFriendly->getUnitCosts()));
                const double fHp = std::max(0.0, static_cast<double>(pFriendly->getHp()));
                EnemySiteCounterThreat bestThreat;
                double bestTrade = -1.0;
                double bestRawDmg = 0.0;
                for (const auto & sp : spawns)
                {
                    if (!sp.canMoveAndFire)
                        continue;
                    const double dmg = oracle.baseDamageWithAmmo(*sp.unit, *pFriendly, sp.minR, sp.minR, sp.maxR);
                    if (dmg <= 0.0)
                        continue;
                    const double ret = (known.canMoveAndFire && known.hasWeapons)
                        ? std::max(0.0, oracle.baseDamageWithAmmo(*pFriendly, *sp.unit, 1, 1, 1))
                        : 0.0;
                    const double dealtHp = std::min(fHp, dmg / UNIFIED_PROD_HP_PERCENT_PER_STEP);
                    const double fAfterHp = std::max(0.0, fHp - dealtHp);
                    const double fLoss = (static_cast<double>(fCost) * dealtHp) / UNIFIED_PROD_FULL_HP;
                    const double retHp = fAfterHp > 0.0
                        ? std::min(UNIFIED_PROD_FULL_HP,
                                   (ret * (fAfterHp / UNIFIED_PROD_FULL_HP)) / UNIFIED_PROD_HP_PERCENT_PER_STEP)
                        : 0.0;
                    const double netTrade = fLoss - (static_cast<double>(sp.cost) * retHp) / UNIFIED_PROD_FULL_HP;
                    if (netTrade > bestTrade || (netTrade == bestTrade && dmg > bestRawDmg))
                    {
                        bestTrade = netTrade;
                        bestRawDmg = dmg;
                        bestThreat = EnemySiteCounterThreat{
                            .gridId = sp.gridId,
                            .movementPoints = sp.move,
                            .minRange = sp.minR,
                            .maxRange = sp.maxR,
                            .canMoveAndFire = sp.canMoveAndFire,
                            .cost = sp.cost,
                            .baseDamageVsFriendly = dmg,
                            .friendlyReturnDamage = ret,
                        };
                    }
                }
                if (bestRawDmg > 0.0 && bestTrade > 0.0)
                    input.enemySiteCounters.emplace(std::tuple{siteTile.x, siteTile.y, ki}, bestThreat);
            }
        }

        // 7. Populate unowned capturable properties and own strategic defense values (Com Towers, Power Plants, production sites).
        std::map<QString, int> ownSiteCountByBuilding;
        for (const auto & prop : properties)
        {
            if (prop.canProduce && knowledge.relation(prop.ownerId) == Relation::Own)
                ++ownSiteCountByBuilding[prop.buildingId];
        }
        const bool hasCo = player.getCO(0) != nullptr || player.getCO(1) != nullptr;
        const MilliFunds futureArmyScale = input.nextTurnIncome * std::max<std::int32_t>(1, input.horizonTurns / 2);
        const MilliFunds combinedArmies =
            input.totalFriendlyCombatCapital + input.totalEnemyCombatCapital + futureArmyScale * 2;
        for (const auto & prop : properties)
        {
            if (!prop.capturable)
                continue;
            const Relation rel = knowledge.relation(prop.ownerId);
            if (rel == Relation::Own)
            {
                MilliFunds ownLoss = 0;
                if (map.onMap(prop.x, prop.y))
                {
                    if (Building * pBuilding = map.getTerrain(prop.x, prop.y)->getBuilding())
                    {
                        const std::int32_t bonusPct =
                            std::max<qint32>(0, pBuilding->getOffensiveBonus()) +
                            std::max<qint32>(0, pBuilding->getDefensiveBonus()) +
                            (hasCo ? std::max<qint32>(0, pBuilding->getPowerChargeBonus()) / 2 : 0);
                        if (bonusPct > 0)
                        {
                            ownLoss += static_cast<MilliFunds>(std::llround(
                                static_cast<double>(combinedArmies) *
                                (static_cast<double>(bonusPct) / UNIFIED_PROD_PERCENT_SCALE)));
                        }
                        if (prop.canProduce && input.nextTurnIncome > 0)
                        {
                            const int ownSame = std::max(1, ownSiteCountByBuilding[prop.buildingId]);
                            ownLoss += input.nextTurnIncome / ownSame;
                        }
                        if (prop.objective == ObjectiveKind::Headquarters)
                        {
                            ownLoss += input.totalFriendlyCombatCapital +
                                input.nextTurnIncome * std::max<std::int32_t>(1, input.horizonTurns);
                        }
                    }
                }
                if (ownLoss > 0)
                    input.ownStrategicLoss.emplace(std::pair{prop.x, prop.y}, ownLoss);
                continue;
            }
            if (rel == Relation::Allied)
                continue;
            ProductionCapturableProperty cp;
            cp.tile = {prop.x, prop.y};
            cp.income = toMilliFunds(std::max<qint32>(0, prop.incomePerTurn > 0 ? prop.incomePerTurn : prop.baseIncome));
            cp.owner = rel == Relation::Enemy ? OwnerSign::Enemy : OwnerSign::Neutral;
            cp.claimedTurn = input.horizonTurns;
            cp.blocked = false;
            if (prop.objective == ObjectiveKind::Headquarters && rel == Relation::Enemy)
            {
                MilliFunds ownerCombatCap = 0;
                for (std::size_t ei = 0; ei < liveEnemyCount; ++ei)
                {
                    if (liveEnemyUnits[ei] != nullptr &&
                        liveEnemyUnits[ei]->getOwner() != nullptr &&
                        liveEnemyUnits[ei]->getOwner()->getPlayerID() == prop.ownerId)
                    {
                        ownerCombatCap += static_cast<MilliFunds>(std::llround(
                            static_cast<double>(input.enemies[ei].replacementCost) *
                            (input.enemies[ei].hp / UNIFIED_PROD_FULL_HP)));
                    }
                }
                MilliFunds ownerInc = 0;
                if (prop.ownerId >= 0 && prop.ownerId < map.getPlayerCount())
                {
                    if (Player * pEnemy = map.getPlayer(prop.ownerId))
                        ownerInc = toMilliFunds(std::max<qint32>(0, pEnemy->calcIncome()));
                }
                cp.hqEliminationValue =
                    ownerCombatCap + ownerInc * std::max<std::int32_t>(1, input.horizonTurns);
            }
            if (map.onMap(prop.x, prop.y))
            {
                if (Building * pBuilding = map.getTerrain(prop.x, prop.y)->getBuilding())
                {
                    cp.offensiveBonus = std::max<qint32>(0, pBuilding->getOffensiveBonus());
                    cp.defensiveBonus = std::max<qint32>(0, pBuilding->getDefensiveBonus());
                    if (hasCo)
                        cp.powerChargeBonus = std::max<qint32>(0, pBuilding->getPowerChargeBonus());
                }
            }
            input.properties.push_back(cp);
        }

        // Reserve each other connected enemy ground factory's cheapest ground unit from enemy single-site counter budget.
        {
            std::vector<TilePoint> connectedObjectives;
            for (const auto & s : input.sites)
            {
                if (s.owner == OwnerSign::Ours)
                    connectedObjectives.push_back(s.tile);
            }
            for (const auto & [ki, pFriendly] : liveFriendlyUnits)
            {
                (void)pFriendly;
                const auto & fKnown = knowledge.units()[static_cast<std::size_t>(ki)];
                connectedObjectives.push_back({fKnown.x, fKnown.y});
            }
            for (const auto & cp : input.properties)
            {
                if (cp.owner == OwnerSign::Neutral)
                    connectedObjectives.push_back(cp.tile);
            }
            std::vector<MilliFunds> siteMinBuildableCost(input.sites.size(), 0);
            std::vector<MilliFunds> siteConnectedGroundFloor(input.sites.size(), 0);
            std::map<int, MilliFunds> ownerConnectedGroundSum;
            const std::int32_t maxReachTurns = std::max<std::int32_t>(1, input.horizonTurns - 1);
            for (std::size_t si = 0; si < input.sites.size(); ++si)
            {
                const auto & s = input.sites[si];
                if (s.owner != OwnerSign::Enemy || si >= siteEnemyOwnerId.size() || siteEnemyOwnerId[si] == NO_OWNER)
                    continue;
                MilliFunds minAny = 0;
                MilliFunds minConnectedGround = 0;
                for (const int ti : s.buildableTemplateIndices)
                {
                    if (ti < 0 || ti >= static_cast<int>(input.templates.size()))
                        continue;
                    const auto & tmpl = input.templates[static_cast<std::size_t>(ti)];
                    if (tmpl.cost <= 0)
                        continue;
                    if (minAny == 0 || tmpl.cost < minAny)
                        minAny = tmpl.cost;
                    if (tmpl.movementPoints <= 0 ||
                        (tmpl.unitType & (GameEnums::UnitType_Air | GameEnums::UnitType_Naval)) != 0 ||
                        tmpl.gridId < 0 || tmpl.gridId >= static_cast<int>(input.grids.size()) ||
                        connectedObjectives.empty())
                        continue;
                    if (minConnectedGround > 0 && tmpl.cost >= minConnectedGround)
                        continue;
                    const auto fwd = computeForwardGridDist(input.grids[static_cast<std::size_t>(tmpl.gridId)], s.tile);
                    const std::int32_t maxCost = maxReachTurns * tmpl.movementPoints;
                    const bool reaches = std::ranges::any_of(connectedObjectives, [&](TilePoint obj) {
                        return lookupGridCost(fwd, obj) <= maxCost;
                    });
                    if (reaches)
                        minConnectedGround = tmpl.cost;
                }
                siteMinBuildableCost[si] = minAny;
                siteConnectedGroundFloor[si] = minConnectedGround;
                if (minConnectedGround > 0)
                    ownerConnectedGroundSum[siteEnemyOwnerId[si]] += minConnectedGround;
            }
            for (std::size_t si = 0; si < input.sites.size(); ++si)
            {
                auto & s = input.sites[si];
                if (s.owner != OwnerSign::Enemy || s.ownerAvailableFunds <= 0 ||
                    si >= siteEnemyOwnerId.size() || siteEnemyOwnerId[si] == NO_OWNER)
                    continue;
                const MilliFunds otherFloor = std::max<MilliFunds>(
                    0, ownerConnectedGroundSum[siteEnemyOwnerId[si]] - siteConnectedGroundFloor[si]);
                s.ownerAvailableFunds = std::max(siteMinBuildableCost[si], s.ownerAvailableFunds - otherFloor);
            }
        }

        // Compute each enemy's 1-turn threat using actual live friendly units and capture income rate.
        MilliFunds minPropIncome = 0;
        for (const auto & prop : input.properties)
        {
            if (prop.income > 0 && (minPropIncome == 0 || prop.income < minPropIncome))
                minPropIncome = prop.income;
        }
        for (std::size_t ei = 0; ei < input.enemies.size(); ++ei)
        {
            auto & eu = input.enemies[ei];
            Unit * pEnemy = liveEnemyUnits[ei];
            if (pEnemy == nullptr || eu.hp <= 0.0)
                continue;
            ProductionEnemyUnit fullEnemy = eu;
            fullEnemy.hp = UNIFIED_PROD_FULL_HP;
            MilliFunds threatSum = 0;
            std::int32_t threatCount = 0;
            for (const auto & [ki, pFriendly] : liveFriendlyUnits)
            {
                const auto & fKnown = knowledge.units()[static_cast<std::size_t>(ki)];
                ProductionUnitTemplate fTmpl;
                fTmpl.cost = toMilliFunds(std::max<qint32>(0, pFriendly->getUnitCosts()));
                if (fTmpl.cost <= 0)
                    continue;
                fTmpl.canMoveAndFire = fKnown.canMoveAndFire;
                const double eStrike = std::max(0.0, oracle.baseDamageWithAmmo(
                    *pEnemy, *pFriendly, eu.minRange, eu.minRange, eu.maxRange));
                if (eStrike <= 0.0)
                    continue;
                const double ourCounter = (eu.canMoveAndFire && fKnown.canMoveAndFire && fKnown.hasWeapons)
                    ? std::max(0.0, oracle.baseDamageWithAmmo(*pFriendly, *pEnemy, 1, 1, 1))
                    : 0.0;
                const MilliFunds strikeThreat = static_cast<MilliFunds>(std::llround(
                    UnifiedProductionModel::enemyFirstStrikeNetThreat(fTmpl, fullEnemy, eStrike, ourCounter)));
                if (strikeThreat > 0)
                {
                    threatSum += strikeThreat;
                    ++threatCount;
                }
            }
            MilliFunds bestThreat = threatCount > 0 ? (threatSum / threatCount) : 0;
            if (eu.canCapture && eu.captureRate > 0 && minPropIncome > 0)
            {
                const std::int32_t capTurns = std::max<std::int32_t>(
                    1, (input.capturePointsToCapture + eu.captureRate - 1) / eu.captureRate);
                bestThreat = std::max(bestThreat, minPropIncome / capTurns);
            }
            eu.perTurnThreat = bestThreat;
        }

        // 7b. Compute exact live-map E0 strike, capper reach onto own sites, and full 1-turn reachable threat mask.
        const std::size_t mapCells =
            static_cast<std::size_t>(std::max<std::int32_t>(0, input.width)) *
            static_cast<std::size_t>(std::max<std::int32_t>(0, input.height));
        std::vector<std::uint8_t> enemyOneTurnThreatMask(mapCells, 0);
        const auto maskIdxOf = [&](TilePoint t) {
            return static_cast<std::size_t>(t.y) * static_cast<std::size_t>(input.width) +
                static_cast<std::size_t>(t.x);
        };
        if (input.width > 0 && input.height > 0)
        {
            std::vector<std::uint8_t> friendlyOcc(mapCells, 0);
            std::vector<std::uint8_t> enemyOcc(mapCells, 0);
            for (const auto & known : knowledge.units())
            {
                if (known.carrier != KnownUnit::NO_CARRIER || known.hpRounded <= 0 || !map.onMap(known.x, known.y))
                    continue;
                const Relation rel = knowledge.relation(known.ownerId);
                if (rel == Relation::Own || rel == Relation::Allied)
                    friendlyOcc[maskIdxOf({known.x, known.y})] = 1;
                else if (rel == Relation::Enemy)
                    enemyOcc[maskIdxOf({known.x, known.y})] = 1;
            }
            for (auto & site : input.sites)
            {
                if (site.owner != OwnerSign::Ours)
                    continue;
                site.enemyCanStrikeInE0.assign(input.enemies.size(), 0);
                site.enemyCapperCanStepInE0.assign(input.enemies.size(), 0);
            }
            const auto markWeaponRing = [&](TilePoint firingTile, std::int32_t minR, std::int32_t maxR,
                                            std::size_t ei, bool markMask) {
                const int x0 = std::max(0, firingTile.x - maxR);
                const int x1 = std::min(input.width - 1, firingTile.x + maxR);
                const int y0 = std::max(0, firingTile.y - maxR);
                const int y1 = std::min(input.height - 1, firingTile.y + maxR);
                for (int y = y0; y <= y1; ++y)
                {
                    for (int x = x0; x <= x1; ++x)
                    {
                        const TilePoint target{x, y};
                        const int md = manhattanDistance(firingTile, target);
                        if (md < minR || md > maxR)
                            continue;
                        if (markMask)
                            enemyOneTurnThreatMask[maskIdxOf(target)] = 1;
                        if (target != firingTile)
                        {
                            for (auto & site : input.sites)
                            {
                                if (site.owner == OwnerSign::Ours && site.tile == target)
                                    site.enemyCanStrikeInE0[ei] = 1;
                            }
                        }
                    }
                }
            };
            for (std::size_t ei = 0; ei < input.enemies.size(); ++ei)
            {
                const auto & eu = input.enemies[ei];
                Unit * pEnemy = liveEnemyUnits[ei];
                if (eu.isProjected || eu.spawnDelayTurns > 0 || eu.hp <= 0.0 ||
                    pEnemy == nullptr || pEnemy->getOwner() == nullptr)
                    continue;
                const bool hasWep = pEnemy->hasWeapons();
                if (!eu.canMoveAndFire || eu.movementPoints <= 0)
                {
                    if (hasWep)
                        markWeaponRing(eu.tile, eu.minRange, eu.maxRange, ei, true);
                    if (eu.canCapture && eu.captureRate > 0)
                    {
                        for (auto & site : input.sites)
                        {
                            if (site.owner == OwnerSign::Ours && site.tile == eu.tile)
                                site.enemyCapperCanStepInE0[ei] = 1;
                        }
                    }
                    continue;
                }
                const MobilityCostGrid & eGrid = mobilityFields.grid(map, *pEnemy->getOwner(), pEnemy->getUnitID());
                if (!eGrid.onGrid(eu.tile.x, eu.tile.y))
                    continue;
                std::vector<std::int32_t> dist(mapCells, std::numeric_limits<std::int32_t>::max());
                using Node = std::pair<std::int32_t, TilePoint>;
                const auto cmp = [](const Node & a, const Node & b) { return a.first > b.first; };
                std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
                dist[maskIdxOf(eu.tile)] = 0;
                pq.push({0, eu.tile});
                while (!pq.empty())
                {
                    const auto [d, cur] = pq.top();
                    pq.pop();
                    if (d > dist[maskIdxOf(cur)])
                        continue;
                    if (eu.canCapture && eu.captureRate > 0)
                    {
                        for (auto & site : input.sites)
                        {
                            if (site.owner == OwnerSign::Ours && site.tile == cur)
                                site.enemyCapperCanStepInE0[ei] = 1;
                        }
                    }
                    const bool occupiedByPeer = cur != eu.tile && enemyOcc[maskIdxOf(cur)] != 0;
                    const bool occupiedByFriend = friendlyOcc[maskIdxOf(cur)] != 0;
                    if (hasWep && !occupiedByPeer && !occupiedByFriend)
                        markWeaponRing(cur, eu.minRange, eu.maxRange, ei, true);
                    for (const EntryDirection dir : ENTRY_DIRECTIONS)
                    {
                        const StepVector step = entryStep(dir);
                        const TilePoint next{cur.x + step.dx, cur.y + step.dy};
                        if (!eGrid.onGrid(next.x, next.y))
                            continue;
                        if (friendlyOcc[maskIdxOf(next)] != 0)
                            continue;
                        const std::int32_t c = eGrid.cost(next.x, next.y, dir);
                        if (c == IMPASSABLE_COST)
                            continue;
                        const std::int32_t nd = d + c;
                        if (nd <= eu.movementPoints && nd < dist[maskIdxOf(next)])
                        {
                            dist[maskIdxOf(next)] = nd;
                            pq.push({nd, next});
                        }
                    }
                }
            }
        }

        // 8. Grid-routed territory masking and simultaneous earliest-ownTurn live-capper claims.
        if (!input.properties.empty())
        {
            const std::int32_t horizon = input.horizonTurns;
            const std::int32_t capPts = input.capturePointsToCapture;

            const MobilityCostGrid * pCapperGrid = nullptr;
            for (const auto & tmpl : input.templates)
            {
                if (tmpl.canCapture && tmpl.gridId >= 0 && tmpl.gridId < static_cast<int>(input.grids.size()))
                {
                    pCapperGrid = &input.grids[static_cast<std::size_t>(tmpl.gridId)];
                    break;
                }
            }
            std::optional<MobilityCostGrid> fallbackInfGrid;
            if (pCapperGrid == nullptr && !liveFriendlyUnits.empty())
            {
                for (const auto & [ki, pFriendly] : liveFriendlyUnits)
                {
                    const auto & known = knowledge.units()[static_cast<std::size_t>(ki)];
                    if (known.canCapture)
                    {
                        fallbackInfGrid = mobilityFields.grid(map, player, known.unitId);
                        pCapperGrid = &*fallbackInfGrid;
                        break;
                    }
                }
            }

            if (pCapperGrid != nullptr)
            {
                struct AnchorInfo
                {
                    TilePoint tile{INVALID_TILE};
                    std::int32_t delayTurns{0};
                    std::int32_t movePts{1};
                };
                std::vector<AnchorInfo> ownAnchors;
                std::vector<AnchorInfo> enemyAnchors;
                std::int32_t defaultCapperMove = 0;
                for (const auto & tmpl : input.templates)
                {
                    if (tmpl.canCapture && tmpl.movementPoints > 0)
                        defaultCapperMove = std::max(defaultCapperMove, tmpl.movementPoints);
                }
                if (defaultCapperMove > 0)
                {
                    for (const auto & s : input.sites)
                    {
                        std::int32_t siteCapMove = 0;
                        for (const int ti : s.buildableTemplateIndices)
                        {
                            if (ti >= 0 && ti < static_cast<int>(input.templates.size()) &&
                                input.templates[static_cast<std::size_t>(ti)].canCapture)
                            {
                                siteCapMove = std::max(
                                    siteCapMove, input.templates[static_cast<std::size_t>(ti)].movementPoints);
                            }
                        }
                        if (s.owner == OwnerSign::Ours && siteCapMove > 0)
                            ownAnchors.push_back({s.tile, 1, siteCapMove});
                        else if (s.owner == OwnerSign::Enemy)
                            enemyAnchors.push_back({s.tile, 1, siteCapMove > 0 ? siteCapMove : defaultCapperMove});
                    }
                }
                for (const auto & [ki, pFriendly] : liveFriendlyUnits)
                {
                    const auto & known = knowledge.units()[static_cast<std::size_t>(ki)];
                    if (known.canCapture && known.movementPoints > 0)
                        ownAnchors.push_back({{known.x, known.y}, known.hasMoved ? 1 : 0, known.movementPoints});
                }
                for (int ki = 0; ki < static_cast<int>(knowledge.units().size()); ++ki)
                {
                    const auto & known = knowledge.units()[static_cast<std::size_t>(ki)];
                    if (known.carrier == KnownUnit::NO_CARRIER && known.hpRounded > 0 &&
                        knowledge.relation(known.ownerId) == Relation::Enemy && known.canCapture &&
                        known.movementPoints > 0)
                    {
                        enemyAnchors.push_back({{known.x, known.y}, 1, known.movementPoints});
                    }
                }
                std::map<std::pair<std::int32_t, std::int32_t>, std::vector<std::int32_t>> anchorFwdCache;
                const auto minAnchorTurns = [&](std::span<const AnchorInfo> anchors, TilePoint target) {
                    std::int32_t best = UNIFIED_PROD_UNREACHABLE_TURNS;
                    for (const auto & a : anchors)
                    {
                        const auto key = std::pair{a.tile.x, a.tile.y};
                        auto it = anchorFwdCache.find(key);
                        if (it == anchorFwdCache.end())
                            it = anchorFwdCache.emplace(key, computeForwardGridDist(*pCapperGrid, a.tile)).first;
                        const std::int32_t c = lookupGridCost(it->second, target);
                        if (c == std::numeric_limits<std::int32_t>::max())
                            continue;
                        const std::int32_t turns = a.delayTurns + (c + a.movePts - 1) / std::max<std::int32_t>(1, a.movePts);
                        best = std::min(best, turns);
                    }
                    return best;
                };
                for (auto & cp : input.properties)
                {
                    if (cp.owner != OwnerSign::Neutral || enemyAnchors.empty() || ownAnchors.empty())
                        continue;
                    const std::int32_t minOwn = minAnchorTurns(ownAnchors, cp.tile);
                    const std::int32_t minEnemy = minAnchorTurns(enemyAnchors, cp.tile);
                    if (minEnemy < UNIFIED_PROD_UNREACHABLE_TURNS && minEnemy + 1 < minOwn)
                        cp.blocked = true;
                }

                // Block enemy-owned properties whose live enemy guard capital at arrival exceeds reachable friendly escort capital.
                const auto minTurnsToStrikeTile = [&](const std::vector<std::int32_t> & fwd, TilePoint origin,
                                                      std::int32_t movePts, bool canMoveFire,
                                                      std::int32_t minR, std::int32_t maxR,
                                                      TilePoint target) -> std::int32_t {
                    const std::int32_t effMin = std::max<std::int32_t>(1, minR);
                    const std::int32_t effMax = std::max(effMin, maxR);
                    const int md = manhattanDistance(origin, target);
                    if (md >= effMin && md <= effMax)
                        return 1;
                    if (movePts <= 0 || fwd.empty())
                        return UNIFIED_PROD_UNREACHABLE_TURNS;
                    const int x0 = std::max(0, target.x - effMax);
                    const int x1 = std::min(input.width - 1, target.x + effMax);
                    const int y0 = std::max(0, target.y - effMax);
                    const int y1 = std::min(input.height - 1, target.y + effMax);
                    std::int32_t bestCost = std::numeric_limits<std::int32_t>::max();
                    for (int y = y0; y <= y1; ++y)
                    {
                        for (int x = x0; x <= x1; ++x)
                        {
                            const TilePoint p{x, y};
                            const int d = manhattanDistance(p, target);
                            if (d < effMin || d > effMax)
                                continue;
                            bestCost = std::min(bestCost, lookupGridCost(fwd, p));
                        }
                    }
                    if (bestCost == std::numeric_limits<std::int32_t>::max())
                        return UNIFIED_PROD_UNREACHABLE_TURNS;
                    const std::int32_t mt = std::max<std::int32_t>(1, (bestCost + movePts - 1) / movePts);
                    return canMoveFire ? mt : (mt + 1);
                };

                struct LiveGuardState
                {
                    std::size_t ei{0};
                    MilliFunds liveCap{0};
                    std::vector<std::int32_t> fwd;
                };
                std::vector<LiveGuardState> liveGuards;
                for (std::size_t ei = 0; ei < input.enemies.size(); ++ei)
                {
                    const auto & eu = input.enemies[ei];
                    Unit * pEnemy = ei < liveEnemyUnits.size() ? liveEnemyUnits[ei] : nullptr;
                    if (eu.isProjected || eu.spawnDelayTurns > 0 || eu.hp <= 0.0 ||
                        pEnemy == nullptr || pEnemy->getOwner() == nullptr)
                        continue;
                    const MilliFunds liveCap = static_cast<MilliFunds>(std::llround(
                        (static_cast<double>(eu.replacementCost) * eu.hp) / UNIFIED_PROD_FULL_HP));
                    if (liveCap <= 0)
                        continue;
                    std::vector<std::int32_t> fwd;
                    if (eu.movementPoints > 0 && pEnemy->hasWeapons())
                    {
                        const MobilityCostGrid & eGrid = mobilityFields.grid(map, *pEnemy->getOwner(), pEnemy->getUnitID());
                        fwd = computeForwardGridDist(eGrid, eu.tile);
                    }
                    liveGuards.push_back({ei, liveCap, std::move(fwd)});
                }

                struct LiveEscortState
                {
                    Unit * unit{nullptr};
                    KnownUnit known;
                    MilliFunds cost{0};
                    MilliFunds liveCap{0};
                    std::vector<std::int32_t> fwd;
                };
                std::vector<LiveEscortState> liveEscorts;
                for (const auto & [ki, pFriendly] : liveFriendlyUnits)
                {
                    const auto & fKnown = knowledge.units()[static_cast<std::size_t>(ki)];
                    if (!fKnown.hasWeapons || pFriendly->getHp() <= 0.0)
                        continue;
                    const qint32 rawCost = player.getCosts(fKnown.unitId, QPoint(fKnown.x, fKnown.y));
                    const MilliFunds fullCost = toMilliFunds(std::max<qint32>(0, rawCost > 0 ? rawCost : pFriendly->getUnitCosts()));
                    const MilliFunds liveCap = static_cast<MilliFunds>(std::llround(
                        (static_cast<double>(fullCost) * static_cast<double>(pFriendly->getHp())) / UNIFIED_PROD_FULL_HP));
                    if (liveCap <= 0)
                        continue;
                    std::vector<std::int32_t> fwd;
                    if (fKnown.movementPoints > 0)
                    {
                        const MobilityCostGrid & fGrid = mobilityFields.grid(map, player, fKnown.unitId);
                        fwd = computeForwardGridDist(fGrid, {fKnown.x, fKnown.y});
                    }
                    liveEscorts.push_back({pFriendly, fKnown, fullCost, liveCap, std::move(fwd)});
                }

                if (!liveGuards.empty() && !ownAnchors.empty())
                {
                    for (auto & cp : input.properties)
                    {
                        if (cp.blocked || cp.owner != OwnerSign::Enemy)
                            continue;
                        const bool midCaptureByOurs = std::ranges::any_of(liveFriendlyUnits, [&](const auto & pair) {
                            const auto & fk = knowledge.units()[static_cast<std::size_t>(pair.first)];
                            return fk.capturePoints > 0 && fk.x == cp.tile.x && fk.y == cp.tile.y;
                        });
                        if (midCaptureByOurs)
                            continue;
                        const std::int32_t capArrive = minAnchorTurns(ownAnchors, cp.tile);
                        if (capArrive >= UNIFIED_PROD_UNREACHABLE_TURNS)
                        {
                            cp.blocked = true;
                            continue;
                        }
                        const std::int32_t capFinish = std::min<std::int32_t>(horizon, std::max<std::int32_t>(1, capArrive) + 1);
                        MilliFunds enemyGuardCap = 0;
                        std::vector<std::size_t> activeGuardEnemyIdx;
                        for (const auto & g : liveGuards)
                        {
                            const auto & eu = input.enemies[g.ei];
                            Unit * pEnemy = liveEnemyUnits[g.ei];
                            if (eu.tile == cp.tile)
                            {
                                enemyGuardCap += g.liveCap;
                                activeGuardEnemyIdx.push_back(g.ei);
                                continue;
                            }
                            if (!pEnemy->hasWeapons())
                                continue;
                            const std::int32_t tStrike = minTurnsToStrikeTile(
                                g.fwd, eu.tile, eu.movementPoints, eu.canMoveAndFire,
                                eu.minRange, eu.maxRange, cp.tile);
                            if (tStrike <= capFinish)
                            {
                                enemyGuardCap += g.liveCap;
                                activeGuardEnemyIdx.push_back(g.ei);
                            }
                        }
                        if (enemyGuardCap <= 0)
                            continue;
                        MilliFunds friendlyEscortCap = 0;
                        for (const auto & esc : liveEscorts)
                        {
                            const std::int32_t fMinR = std::max<std::int32_t>(1, esc.known.minRange);
                            const std::int32_t fMaxR = std::max(fMinR, esc.known.maxRange);
                            const std::int32_t tEscort = minTurnsToStrikeTile(
                                esc.fwd, {esc.known.x, esc.known.y}, esc.known.movementPoints,
                                esc.known.canMoveAndFire, fMinR, fMaxR, cp.tile);
                            if (tEscort > capFinish)
                                continue;
                            bool effectiveEscort = !esc.known.canCapture;
                            if (!effectiveEscort)
                            {
                                ProductionUnitTemplate fTmpl;
                                fTmpl.cost = esc.cost;
                                fTmpl.minRange = fMinR;
                                fTmpl.maxRange = fMaxR;
                                fTmpl.canMoveAndFire = esc.known.canMoveAndFire;
                                fTmpl.hasWeapons = true;
                                for (const std::size_t gei : activeGuardEnemyIdx)
                                {
                                    Unit * pGuard = liveEnemyUnits[gei];
                                    const auto & geu = input.enemies[gei];
                                    const double bDmg = oracle.baseDamageWithAmmo(
                                        *esc.unit, *pGuard, fMinR, fMinR, fMaxR) *
                                        (static_cast<double>(esc.unit->getHp()) / UNIFIED_PROD_FULL_HP);
                                    const double rDmg = (esc.known.canMoveAndFire && geu.canMoveAndFire && fMinR == 1 && geu.minRange == 1)
                                        ? std::max(0.0, oracle.baseDamageWithAmmo(*pGuard, *esc.unit, 1, 1, 1))
                                        : 0.0;
                                    if (UnifiedProductionModel::firstStrikeNetTrade(fTmpl, geu, bDmg, rDmg, fMinR) > 0.0)
                                    {
                                        effectiveEscort = true;
                                        break;
                                    }
                                }
                            }
                            if (effectiveEscort)
                                friendlyEscortCap += esc.liveCap;
                        }
                        if (enemyGuardCap > friendlyEscortCap)
                            cp.blocked = true;
                    }
                }
            }

            const bool hasCombatPresence =
                input.totalFriendlyCombatCapital > 0 || input.friendlyDirectCapital > 0;
            if (!hasCombatPresence)
            {
                for (auto & cp : input.properties)
                {
                    if (cp.blocked || cp.tile.x < 0 || cp.tile.y < 0 ||
                        cp.tile.x >= input.width || cp.tile.y >= input.height)
                        continue;
                    if (enemyOneTurnThreatMask[maskIdxOf(cp.tile)] != 0)
                        cp.blocked = true;
                }
            }
            std::vector<ActiveLiveCapper> ownLiveCappers;
            for (const auto & [ki, pFriendly] : liveFriendlyUnits)
            {
                const auto & known = knowledge.units()[static_cast<std::size_t>(ki)];
                if (!known.canCapture || known.captureRate <= 0 || known.movementPoints <= 0)
                    continue;
                const int move = std::max<int>(1, known.movementPoints);
                const int capTurns = (capPts + known.captureRate - 1) / known.captureRate;
                TilePoint curTile{known.x, known.y};
                std::int32_t curTurn = known.hasMoved ? 1 : 0;
                if (known.capturePoints > 0)
                {
                    for (auto & cp : input.properties)
                    {
                        if (cp.tile == curTile)
                        {
                            const int remCap = std::max<int>(
                                1, (capPts - known.capturePoints + known.captureRate - 1) / known.captureRate);
                            const std::int32_t ownTurn = curTurn + remCap;
                            cp.claimedTurn = std::min<std::int32_t>(cp.claimedTurn, ownTurn);
                            curTurn = ownTurn;
                            break;
                        }
                    }
                }
                if (known.hpRounded >= 1 && curTurn < horizon - 1)
                {
                    ownLiveCappers.push_back(ActiveLiveCapper{
                        .curTile = curTile,
                        .curTurn = curTurn,
                        .move = move,
                        .capTurns = capTurns,
                        .grid = &mobilityFields.grid(map, player, known.unitId),
                        .fwd = {},
                        .fwdDirty = true,
                        .busyInitial = (known.capturePoints > 0),
                    });
                }
            }
            const std::int32_t busyOwnCappers = claimLiveCappersSimultaneously(
                horizon, ownLiveCappers, input.properties, &enemyOneTurnThreatMask, hasCombatPresence);
            input.friendlyScreenBodies = std::max<std::int32_t>(0, totalMobileCappers - busyOwnCappers);
        }

        return UnifiedProductionModel(std::move(input));
    }
}
