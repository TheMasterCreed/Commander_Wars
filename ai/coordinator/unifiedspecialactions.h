#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/economicledger.h"
#include "ai/coordinator/ownershipschedule.h"
#include "ai/coordinator/unifiedproduction.h"
#include "game/GameEnums.h"

namespace Coordinator
{
    inline constexpr std::int32_t UNIFIED_SILO_BLAST_RADIUS = 2;
    inline constexpr std::int32_t UNIFIED_SILO_DAMAGE_STEPS = 3;
    inline constexpr std::int32_t UNIFIED_NONLETHAL_MIN_HP_STEPS = 1;
    inline constexpr std::int32_t UNIFIED_BLACKHOLE_DOOR1_DX = -2;
    inline constexpr std::int32_t UNIFIED_BLACKHOLE_DOOR2_DX = -1;
    inline constexpr std::int32_t UNIFIED_BLACKHOLE_DOOR3_DX = 0;

    struct SpecialActionUnit
    {
        TilePoint tile{INVALID_TILE};
        MilliFunds replacementCost{0};
        std::int32_t hpSteps{0};
        bool isEnemy{false};
        bool hasMoved{false};
        bool needsSupplyRefill{false};
        std::int32_t engineUnitId{NO_UNIT};
    };

    struct SiloStrikeQuote
    {
        TilePoint targetCenter{INVALID_TILE};
        MilliFunds netValue{0};
        MilliFunds enemyLoss{0};
        MilliFunds friendlyLoss{0};
        bool hitsUnmovedFriendly{false};
        bool killsOrCripplesEnemy{false};
    };

    struct AreaBuildingActionQuote
    {
        MilliFunds netValue{0};
        MilliFunds enemyLoss{0};
        MilliFunds friendlyLoss{0};
        MilliFunds maxEnemyUnitCost{0};
        bool killsOrCripplesEnemy{false};
        bool refillsFullHpFriendly{false};
    };

    // Respects engine specialDestruction rule (non-lethal special weapons floor at 1 HP step when false).
    inline MilliFunds specialDamageCapitalDelta(MilliFunds replacementCost,
                                                std::int32_t hpStepsBefore,
                                                std::int32_t damageSteps,
                                                bool specialDestruction)
    {
        if (replacementCost <= 0 || hpStepsBefore <= 0 || damageSteps <= 0)
        {
            return 0;
        }
        const std::int32_t minAfter = specialDestruction ? 0 : UNIFIED_NONLETHAL_MIN_HP_STEPS;
        const std::int32_t hpAfter = std::max(minAfter, hpStepsBefore - damageSteps);
        const std::int32_t lostSteps = std::max<std::int32_t>(0, hpStepsBefore - hpAfter);
        return (replacementCost * lostSteps) / UNIT_HP_STEPS;
    }

    inline SiloStrikeQuote evaluateBestSiloStrike(int mapWidth,
                                                  int mapHeight,
                                                  TilePoint actorOrigin,
                                                  TilePoint siloTile,
                                                  MilliFunds actorCost,
                                                  std::int32_t actorHpSteps,
                                                  std::span<const SpecialActionUnit> boardUnits,
                                                  bool specialDestruction,
                                                  std::int32_t damageSteps = UNIFIED_SILO_DAMAGE_STEPS,
                                                  std::int32_t blastRadius = UNIFIED_SILO_BLAST_RADIUS)
    {
        SiloStrikeQuote best;
        if (mapWidth <= 0 || mapHeight <= 0 || damageSteps <= 0 || blastRadius < 0)
        {
            return best;
        }
        std::vector<SpecialActionUnit> postMoveUnits;
        postMoveUnits.reserve(boardUnits.size() + 1);
        bool hasEnemy = false;
        for (const auto & unit : boardUnits)
        {
            if (unit.hpSteps <= 0 || unit.tile == INVALID_TILE || unit.tile == actorOrigin)
            {
                continue;
            }
            if (unit.isEnemy)
            {
                hasEnemy = true;
            }
            postMoveUnits.push_back(unit);
        }
        if (!hasEnemy)
        {
            return best;
        }
        postMoveUnits.push_back(SpecialActionUnit{
            .tile = siloTile,
            .replacementCost = actorCost,
            .hpSteps = actorHpSteps,
            .isEnemy = false,
            .hasMoved = true,
            .needsSupplyRefill = false,
            .engineUnitId = NO_UNIT,
        });

        std::vector<TilePoint> candidates;
        for (const auto & unit : postMoveUnits)
        {
            if (!unit.isEnemy)
            {
                continue;
            }
            for (int dy = -blastRadius; dy <= blastRadius; ++dy)
            {
                const int maxDx = blastRadius - std::abs(dy);
                for (int dx = -maxDx; dx <= maxDx; ++dx)
                {
                    const TilePoint c{unit.tile.x + dx, unit.tile.y + dy};
                    if (c.x >= 0 && c.x < mapWidth && c.y >= 0 && c.y < mapHeight)
                    {
                        candidates.push_back(c);
                    }
                }
            }
        }
        std::ranges::sort(candidates, [](TilePoint a, TilePoint b) {
            return std::tie(a.y, a.x) < std::tie(b.y, b.x);
        });
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        for (const TilePoint center : candidates)
        {
            MilliFunds enemyLoss = 0;
            MilliFunds friendlyLoss = 0;
            bool hitsUnmovedFriendly = false;
            bool killsOrCripples = false;
            for (const auto & unit : postMoveUnits)
            {
                if (manhattanDistance(unit.tile, center) > blastRadius)
                {
                    continue;
                }
                const MilliFunds delta = specialDamageCapitalDelta(
                    unit.replacementCost, unit.hpSteps, damageSteps, specialDestruction);
                if (unit.isEnemy)
                {
                    enemyLoss += delta;
                    const std::int32_t minAfter = specialDestruction ? 0 : UNIFIED_NONLETHAL_MIN_HP_STEPS;
                    if (std::max(minAfter, unit.hpSteps - damageSteps) <= UNIFIED_NONLETHAL_MIN_HP_STEPS)
                    {
                        killsOrCripples = true;
                    }
                }
                else
                {
                    friendlyLoss += delta;
                    if (!unit.hasMoved && unit.tile != siloTile && delta > 0)
                    {
                        hitsUnmovedFriendly = true;
                    }
                }
            }
            const MilliFunds net = enemyLoss - friendlyLoss;
            const bool better = net > best.netValue ||
                (net == best.netValue && net > 0 &&
                 std::tuple{-friendlyLoss, enemyLoss, -center.y, -center.x} >
                     std::tuple{-best.friendlyLoss, best.enemyLoss, -best.targetCenter.y, -best.targetCenter.x});
            if (better)
            {
                best.targetCenter = center;
                best.netValue = net;
                best.enemyLoss = enemyLoss;
                best.friendlyLoss = friendlyLoss;
                best.hitsUnmovedFriendly = hitsUnmovedFriendly;
                best.killsOrCripplesEnemy = killsOrCripples;
            }
        }
        return best;
    }

    inline AreaBuildingActionQuote evaluateAreaBuildingAction(std::span<const TilePoint> targetTiles,
                                                            std::span<const SpecialActionUnit> boardUnits,
                                                            std::int32_t damageSteps,
                                                            GameEnums::BuildingTarget targetMode,
                                                            bool specialDestruction)
    {
        AreaBuildingActionQuote quote;
        if (targetTiles.empty() || damageSteps == 0)
        {
            return quote;
        }
        for (const auto & unit : boardUnits)
        {
            if (unit.hpSteps <= 0 || unit.tile == INVALID_TILE)
            {
                continue;
            }
            const bool inArea = std::ranges::any_of(targetTiles, [&](TilePoint t) { return t == unit.tile; });
            if (!inArea)
            {
                continue;
            }
            if (damageSteps < 0)
            {
                if (unit.isEnemy || targetMode == GameEnums::BuildingTarget_Enemy)
                {
                    continue;
                }
                const std::int32_t healSteps = std::min<std::int32_t>(
                    std::max<std::int32_t>(0, UNIT_HP_STEPS - unit.hpSteps), -damageSteps);
                quote.netValue += (unit.replacementCost * healSteps) / UNIT_HP_STEPS;
                if (healSteps == 0 && unit.needsSupplyRefill)
                {
                    quote.refillsFullHpFriendly = true;
                }
                continue;
            }
            if (unit.isEnemy)
            {
                if (targetMode == GameEnums::BuildingTarget_Own)
                {
                    continue;
                }
                const MilliFunds loss = specialDamageCapitalDelta(
                    unit.replacementCost, unit.hpSteps, damageSteps, specialDestruction);
                quote.enemyLoss += loss;
                quote.maxEnemyUnitCost = std::max(quote.maxEnemyUnitCost, unit.replacementCost);
                const std::int32_t minAfter = specialDestruction ? 0 : UNIFIED_NONLETHAL_MIN_HP_STEPS;
                if (std::max(minAfter, unit.hpSteps - damageSteps) <= UNIFIED_NONLETHAL_MIN_HP_STEPS)
                {
                    quote.killsOrCripplesEnemy = true;
                }
            }
            else if (targetMode == GameEnums::BuildingTarget_All || targetMode == GameEnums::BuildingTarget_Own)
            {
                quote.friendlyLoss += specialDamageCapitalDelta(
                    unit.replacementCost, unit.hpSteps, damageSteps, specialDestruction);
            }
        }
        if (damageSteps < 0)
        {
            if (quote.netValue <= 0 && quote.refillsFullHpFriendly)
            {
                quote.netValue = 1;
            }
            return quote;
        }
        quote.netValue = quote.enemyLoss - quote.friendlyLoss;
        return quote;
    }

    // Evaluates zero-cost door spawner builds (Black Hole Factory, Nest Factory, Free Oozium) via UnifiedProductionModel.
    inline QString selectZeroCostDoorUnit(const UnifiedProductionModel & baseModel,
                                          TilePoint doorSpawnTile,
                                          const QStringList & legalUnitIds)
    {
        if (legalUnitIds.isEmpty())
        {
            return {};
        }
        if (legalUnitIds.size() == 1 || !baseModel.valid())
        {
            return legalUnitIds.front();
        }
        ProductionModelInput inp = baseModel.input();
        inp.sites.clear();

        UnifiedProductionSite doorSite;
        doorSite.tile = doorSpawnTile;
        doorSite.owner = OwnerSign::Ours;

        MilliFunds maxDoorCost = 0;
        for (int ti = 0; ti < static_cast<int>(inp.templates.size()); ++ti)
        {
            const auto & tmpl = inp.templates[static_cast<std::size_t>(ti)];
            if (legalUnitIds.contains(tmpl.unitId))
            {
                doorSite.buildableTemplateIndices.push_back(ti);
                maxDoorCost = std::max(maxDoorCost, tmpl.cost);
            }
        }
        if (doorSite.buildableTemplateIndices.empty())
        {
            return legalUnitIds.front();
        }
        inp.currentFunds = std::max<MilliFunds>(1, maxDoorCost);
        inp.nextTurnIncome = 0;
        inp.sites.push_back(std::move(doorSite));

        UnifiedProductionModel doorModel(std::move(inp));
        int bestTi = -1;
        MilliFunds bestScore = -1;
        for (const int ti : doorModel.input().sites.front().buildableTemplateIndices)
        {
            const MilliFunds val = doorModel.zeroCostSpawnHorizonValue(doorSpawnTile, ti);
            if (val > bestScore)
            {
                bestScore = val;
                bestTi = ti;
            }
        }
        return bestTi >= 0 ? doorModel.input().templates[static_cast<std::size_t>(bestTi)].unitId : legalUnitIds.front();
    }
}
