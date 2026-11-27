#include "ai/coordinator/replyfactsbuilder.h"

#include <algorithm>
#include <chrono>

#include "ai/coordinator/bundlebuilder.h"

#include "game/building.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"

namespace
{
    using Clock = std::chrono::steady_clock;
    // A counter answers an adjacent shot.
    constexpr std::int32_t COUNTER_DISTANCE = 1;

    Coordinator::CaptureDamageFormula damageFormula(GameEnums::DamageFormula formula)
    {
        switch (formula)
        {
            case GameEnums::DamageFormula_AdvanceWars1_3:
                return Coordinator::CaptureDamageFormula::AdvanceWars1To3;
            case GameEnums::DamageFormula_AdvanceWars4:
                break;
        }
        return Coordinator::CaptureDamageFormula::AdvanceWars4;
    }
}

namespace Coordinator
{
    // building.js repairs and resupplies a unit on its owner's building at that owner's turn start.
    bool ownBuildingAt(GameMap & map, const TilePoint & tile, std::int32_t ownerId)
    {
        Terrain* pTerrain = map.getTerrain(tile.x, tile.y);
        Building* pBuilding = pTerrain == nullptr ? nullptr : pTerrain->getBuilding();
        return pBuilding != nullptr && pBuilding->getOwner() != nullptr &&
               pBuilding->getOwner()->getPlayerID() == ownerId;
    }

    ReplyBoard replyBoardOf(const BattlefieldKnowledge & knowledge)
    {
        ReplyBoard board{.width = knowledge.width(), .height = knowledge.height()};
        const std::vector<KnownUnit> & units = knowledge.units();
        for (std::size_t index = 0; index < units.size(); ++index)
        {
            const KnownUnit & unit = units[index];
            const Relation relation = knowledge.relation(unit.ownerId);
            if (!blocksEnemyFiringTile(relation, unit.carrier != KnownUnit::NO_CARRIER))
            {
                continue;
            }
            if (relation == Relation::Allied)
            {
                ++board.alliedOccupants;
            }
            board.standing.push_back(OccupantAt{static_cast<std::int32_t>(index), TilePoint{unit.x, unit.y}});
        }
        return board;
    }

    ReplyFactsBuilder::ReplyFactsBuilder(GameMap & map, const BattlefieldKnowledge & knowledge,
                                         const AttackOpportunityField & enemyReach,
                                         std::span<const PropertyFacts> properties,
                                         const PropertyStockField & propertyStock, DamageOracle & oracle)
        : m_map(map),
          m_knowledge(knowledge),
          m_enemyReach(enemyReach),
          m_properties(properties),
          m_propertyStock(propertyStock),
          m_oracle(oracle),
          m_emptyBoard(knowledge.width(), knowledge.height(), {}, {})
    {
    }

    const KnownUnit & ReplyFactsBuilder::known(std::int32_t knowledgeIndex) const
    {
        return m_knowledge.units()[static_cast<std::size_t>(knowledgeIndex)];
    }

    std::shared_ptr<const ReplyTileFacts> ReplyFactsBuilder::tileFacts(const TilePoint & target)
    {
        const auto started = Clock::now();
        std::shared_ptr<const ReplyTileFacts> facts = cachedTileFacts(target);
        m_stats.nanos += nanosSince(started);
        return facts;
    }

    std::shared_ptr<const ReplyTileFacts> ReplyFactsBuilder::cachedTileFacts(const TilePoint & target)
    {
        const auto cached = m_tiles.find(TileKey{target.x, target.y});
        if (cached != m_tiles.end())
        {
            return cached->second;
        }
        auto facts = std::make_shared<ReplyTileFacts>();
        facts->target = target;
        forEachEnemyShot(m_knowledge.units(), m_enemyReach, target, NO_UNIT, 0, [&](const EnemyShot & shot)
        {
            if (shot.distance != NO_SHOT_DISTANCE)
            {
                facts->shooters.push_back(shooterFacts(shot, target));
            }
        });
        ++m_stats.tileFacts;
        return m_tiles.emplace(TileKey{target.x, target.y}, std::move(facts)).first->second;
    }

    ReplyShooterFacts ReplyFactsBuilder::shooterFacts(const EnemyShot & shot, const TilePoint & target)
    {
        const AttackerSpec & spec = m_enemyReach.attacker(shot.slot);
        const KnownUnit & unit = known(shot.unitIndex);
        ReplyShooterFacts shooter{
            .slot = shot.slot,
            .shooterIndex = shot.unitIndex,
            .tile = shot.tile,
            .dayStartHpSteps = shot.hpSteps,
            .distance = shot.distance,
            .live = liveUnitFor(m_map, unit) != nullptr,
            .direct = spec.canMoveAndFire,
            .onOwnBuilding = ownBuildingAt(m_map, TilePoint{unit.x, unit.y}, unit.ownerId),
            .mustMove = shooterMustMove(shot.tile, target, spec.minRange, spec.maxRange),
        };
        if (spec.canMoveAndFire)
        {
            const DistanceField & reach = m_enemyReach.reach(shot.slot);
            shooter.firingTiles = orderedFiringTiles(
                FiringRange{.target = target, .minRange = spec.minRange, .maxRange = spec.maxRange}, m_emptyBoard,
                [&](std::int32_t x, std::int32_t y)
                {
                    return reach.reachable(x, y, spec.movementPoints) ? reach.distance(x, y) : UNREACHABLE;
                });
        }
        priceShooter(shooter);
        return shooter;
    }

    // Every shooter is priced at its type's base cost, one scale for all, so no enemy unit runs a script.
    void ReplyFactsBuilder::priceShooter(ReplyShooterFacts & shooter)
    {
        const std::int32_t index = shooter.shooterIndex;
        const KnownUnit & unit = known(index);
        auto base = m_baseCosts.find(unit.unitId);
        if (base == m_baseCosts.end())
        {
            // getBaseCost answers -1 for a type without a cost.
            const std::int32_t funds = std::max(Unit::getBaseCosts(unit.unitId, &m_map), 0);
            base = m_baseCosts.emplace(unit.unitId, toMilliFunds(funds)).first;
        }
        shooter.cost = base->second;
        const TilePoint tile{unit.x, unit.y};
        const std::int32_t propertyIndex = m_propertyStock.propertyIndexAt(tile);
        const std::int32_t slot = m_propertyStock.columnSlotAt(tile);
        if (propertyIndex == NO_PROPERTY_INDEX || slot == NO_STOCK_COLUMN)
        {
            return;
        }
        const PropertyFacts & facts = m_properties[static_cast<std::size_t>(propertyIndex)];
        shooter.interruption = captureInterruption(
            shooterCapturingHere(facts.capturerIndex, facts.capturePoints, index), shooter.mustMove);
        if (shooter.interruption == CaptureInterruption::None)
        {
            return;
        }
        const std::int32_t horizonTurns = m_propertyStock.horizonTurns();
        const PropertyStockColumn & column = m_propertyStock.enemyColumn(slot);
        const std::int32_t points = m_propertyStock.capturePointsToCapture();
        shooter.captureCost = forgoneCaptureStream(column, facts.capturePoints, unit.captureRate, shooter.interruption,
                                                   points, horizonTurns);
        const std::int32_t row = m_propertyStock.enemyRowOf(index);
        if (row != NO_STOCK_ROW)
        {
            shooter.stockAgrees = captureStreamInPlace(column, facts.capturePoints, unit.captureRate, points,
                                                       horizonTurns) == m_propertyStock.enemyWeight(row, slot);
        }
    }

    // useTerrainDefense and getBaseDefense only read scripts; CO terrain modifiers are left out on purpose.
    TerrainDefenseRule projectedTerrainRule(GameMap &m_map, Unit &actor, const TilePoint &tile)
    {
        TerrainDefenseRule rule;
        rule.luck = REPLY_TERRAIN_LUCK;
        GameRules* pRules = m_map.getGameRules();
        Terrain* pTerrain = m_map.getTerrain(tile.x, tile.y);
        if (pRules == nullptr || pTerrain == nullptr)
        {
            return rule;
        }
        rule.defensePerStar = pRules->getTerrainDefense();
        rule.hpScaled = pRules->getHpDefenseReduction();
        rule.formula = damageFormula(pRules->getDamageFormula());
        if (actor.useTerrainDefense())
        {
            rule.stars = pTerrain->getBaseDefense();
        }
        return rule;
    }

    // The counter is a weapon table lookup with our unit as attacker, never an engine damage call.
    std::shared_ptr<const ReplyFacts> ReplyFactsBuilder::factsFor(std::int32_t actorIndex, Unit & actor,
                                                                  const TilePoint & destination)
    {
        const ActorKey key{actorIndex, TileKey{destination.x, destination.y}};
        const auto cached = m_actors.find(key);
        if (cached != m_actors.end())
        {
            return cached->second;
        }
        const auto started = Clock::now();
        auto facts = std::make_shared<ReplyFacts>();
        facts->tile = cachedTileFacts(destination);
        facts->terrain = projectedTerrainRule(m_map,actor, destination);
        facts->actorRealHp = actor.getHp();
        const KnownUnit & actorUnit = known(actorIndex);
        facts->shots.reserve(facts->tile->shooters.size());
        for (const ReplyShooterFacts & shooter : facts->tile->shooters)
        {
            ReplyShotBase shot;
            const KnownUnit & enemy = known(shooter.shooterIndex);
            Unit* pEnemy = shooter.live ? liveUnitFor(m_map, enemy) : nullptr;
            if (pEnemy != nullptr)
            {
                shot.rawBase =
                    m_oracle.baseDamageAgainst(*pEnemy, actor, shooter.distance, enemy.minRange, enemy.maxRange);
                shot.withAmmo = shot.rawBase >= 0
                                    ? shot.rawBase
                                    : m_oracle.baseDamageWithAmmo(*pEnemy, actor, shooter.distance, enemy.minRange,
                                                                  enemy.maxRange);
                if (anyCounterTile(shooter.direct, shooter.firingTiles, destination))
                {
                    shot.counterRawBase = m_oracle.baseDamageAgainst(actor, *pEnemy, COUNTER_DISTANCE,
                                                                     actorUnit.minRange, actorUnit.maxRange);
                }
            }
            facts->shots.push_back(shot);
        }
        ++m_stats.actorFacts;
        std::shared_ptr<const ReplyFacts> result = m_actors.emplace(key, std::move(facts)).first->second;
        m_stats.nanos += nanosSince(started);
        return result;
    }
}
