#include "ai/coordinator/battlefieldknowledge.h"

#include "game/GameEnums.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/weaponrangecheck.h"

namespace
{
    qint32 ownerId(Player* pOwner)
    {
        return pOwner != nullptr ? pOwner->getPlayerID() : Coordinator::NO_OWNER;
    }

    std::vector<Coordinator::Relation> capturePlayerRelations(GameMap & map, Player & observer, qint32 observerId)
    {
        const qint32 playerCount = map.getPlayerCount();
        std::vector<Coordinator::Relation> relations;
        relations.reserve(static_cast<std::size_t>(playerCount));
        for (qint32 player = 0; player < playerCount; ++player)
        {
            Player* pPlayer = map.getPlayer(player);
            if (player == observerId)
            {
                relations.push_back(Coordinator::Relation::Own);
            }
            else if (pPlayer == nullptr)
            {
                relations.push_back(Coordinator::Relation::Neutral);
            }
            else if (observer.checkAlliance(pPlayer) == GameEnums::Alliance_Friend)
            {
                relations.push_back(Coordinator::Relation::Allied);
            }
            else
            {
                relations.push_back(Coordinator::Relation::Enemy);
            }
        }
        return relations;
    }

    // isStealthed is false for own and allied units and runs the terrain getVisionHide scripts.
    bool isUnitKnown(Player & observer, Unit & unit)
    {
        return !unit.isStealthed(&observer);
    }

    // Mirrors UnitPathFindingSystem::setMovepoints, which trims until the fuel cost fits.
    qint32 fuelTrimmedMovementPoints(Unit & unit, const QPoint & position)
    {
        qint32 points = unit.getMovementpoints(position);
        const qint32 fuel = unit.getFuel();
        if (fuel < 0)
        {
            return points;
        }
        while (points > 0 && unit.getMovementFuelCostModifier(points) + points > fuel)
        {
            --points;
        }
        return points;
    }

    bool canFireAnyWeapon(Unit & unit)
    {
        const WeaponRangeCheck::Slot first{!unit.getWeapon1ID().isEmpty(), unit.hasAmmo1()};
        const WeaponRangeCheck::Slot second{!unit.getWeapon2ID().isEmpty(), unit.hasAmmo2()};
        return first.isUsable() || second.isUsable();
    }

    // Cargo keeps the defaults, since its hooks would run against the carrier's tile.
    void addAttackFacts(Coordinator::KnownUnit & known, Unit & unit, qint32 & currentTurnFuelTrimmedCappers)
    {
        const QPoint position(known.x, known.y);
        known.movementPoints = fuelTrimmedMovementPoints(unit, position);
        known.minRange = unit.getMinRange(position);
        known.maxRange = unit.getMaxRange(position);
        known.canMoveAndFire = unit.canMoveAndFire(position);
        known.canFire = canFireAnyWeapon(unit);
        // Gated so CO capture hooks skip non capturers.
        known.canCapture = unit.canCapture();
        if (!known.canCapture)
        {
            return;
        }
        known.captureRate = unit.getCaptureRate(position);
        // Current turn trim only; zero does not imply the multi turn relaxation is inactive.
        if (known.movementPoints < unit.getMovementpoints(position))
        {
            ++currentTurnFuelTrimmedCappers;
        }
    }

    Coordinator::KnownUnit describeUnit(Unit & unit, qint32 x, qint32 y, qint32 carrier)
    {
        return Coordinator::KnownUnit{
            .x = x,
            .y = y,
            .unitId = unit.getUnitID(),
            .ownerId = ownerId(unit.getOwner()),
            .hpRounded = unit.getHpRounded(),
            .ammo1 = unit.getAmmo1(),
            .ammo2 = unit.getAmmo2(),
            .fuel = unit.getFuel(),
            .hasMoved = unit.getHasMoved(),
            .carrier = carrier,
            .unitValue = unit.getCoUnitValue(),
            .hasWeapons = unit.hasWeapons(),
            .capturePoints = unit.getCapturePoints(),
        };
    }

    void appendLoadedUnits(Player & observer, Unit & carrier, qint32 carrierIndex, std::vector<Coordinator::KnownUnit> & units)
    {
        if (carrier.getTransportHidden(&observer))
        {
            return;
        }
        const qint32 loadedCount = carrier.getLoadedUnitCount();
        for (qint32 i = 0; i < loadedCount; ++i)
        {
            Unit* pLoaded = carrier.getLoadedUnit(i);
            if (pLoaded == nullptr)
            {
                continue;
            }
            const qint32 loadedIndex = static_cast<qint32>(units.size());
            units.push_back(describeUnit(*pLoaded, units[static_cast<std::size_t>(carrierIndex)].x,
                                         units[static_cast<std::size_t>(carrierIndex)].y, carrierIndex));
            appendLoadedUnits(observer, *pLoaded, loadedIndex, units);
        }
    }

    // A multi tile building answers on every tile it covers, so only its anchor counts.
    bool isBuildingKnown(Player & observer, Building & building, Terrain & terrain, qint32 x, qint32 y)
    {
        return building.getTerrain() == &terrain &&
               observer.getFieldVisibleType(x, y) != GameEnums::VisionType_Shrouded;
    }

    Coordinator::KnownBuilding describeBuilding(Building & building, qint32 x, qint32 y)
    {
        return Coordinator::KnownBuilding{
            .x = x,
            .y = y,
            .buildingId = building.getBuildingID(),
            .ownerId = ownerId(building.getOwner()),
        };
    }
}

namespace Coordinator
{
    BattlefieldKnowledge BattlefieldKnowledge::capture(GameMap & map, Player & observer)
    {
        BattlefieldKnowledge knowledge;
        knowledge.m_observerId = observer.getPlayerID();
        knowledge.m_width = map.getMapWidth();
        knowledge.m_height = map.getMapHeight();
        const std::size_t tileCount = static_cast<std::size_t>(knowledge.m_width) * static_cast<std::size_t>(knowledge.m_height);
        knowledge.m_unitIndex.assign(tileCount, NO_UNIT);
        knowledge.m_visible.assign(tileCount, false);
        knowledge.m_visionType.assign(tileCount, GameEnums::VisionType_Shrouded);
        knowledge.m_relations = capturePlayerRelations(map, observer, knowledge.m_observerId);
        for (qint32 y = 0; y < knowledge.m_height; ++y)
        {
            for (qint32 x = 0; x < knowledge.m_width; ++x)
            {
                const std::size_t tile = knowledge.tileIndex(x, y);
                knowledge.m_visible[tile] = observer.getFieldVisible(x, y);
                knowledge.m_visionType[tile] = observer.getFieldVisibleType(x, y);
                Terrain* pTerrain = map.getTerrain(x, y);
                if (pTerrain == nullptr)
                {
                    continue;
                }
                Unit* pUnit = pTerrain->getUnit();
                if (pUnit != nullptr && isUnitKnown(observer, *pUnit))
                {
                    const qint32 unitIndex = static_cast<qint32>(knowledge.m_units.size());
                    knowledge.m_unitIndex[tile] = unitIndex;
                    knowledge.m_units.push_back(describeUnit(*pUnit, x, y, KnownUnit::NO_CARRIER));
                    addAttackFacts(knowledge.m_units.back(), *pUnit, knowledge.m_currentTurnFuelTrimmedCappers);
                    appendLoadedUnits(observer, *pUnit, unitIndex, knowledge.m_units);
                }
                Building* pBuilding = pTerrain->getBuilding();
                if (pBuilding != nullptr && isBuildingKnown(observer, *pBuilding, *pTerrain, x, y))
                {
                    knowledge.m_buildings.push_back(describeBuilding(*pBuilding, x, y));
                }
            }
        }
        return knowledge;
    }

    const KnownUnit* BattlefieldKnowledge::unitAt(qint32 x, qint32 y) const
    {
        const qint32 unit = unitIndexAt(x, y);
        if (unit == NO_UNIT)
        {
            return nullptr;
        }
        return &m_units[static_cast<std::size_t>(unit)];
    }

    qint32 BattlefieldKnowledge::unitIndexAt(qint32 x, qint32 y) const
    {
        if (!onMap(x, y))
        {
            return NO_UNIT;
        }
        return m_unitIndex[tileIndex(x, y)];
    }

    bool BattlefieldKnowledge::isVisible(qint32 x, qint32 y) const
    {
        return onMap(x, y) && m_visible[tileIndex(x, y)];
    }

    GameEnums::VisionType BattlefieldKnowledge::visionType(qint32 x, qint32 y) const
    {
        return onMap(x, y) ? m_visionType[tileIndex(x, y)] : GameEnums::VisionType_Shrouded;
    }

    Relation BattlefieldKnowledge::relation(qint32 ownerId) const
    {
        if (ownerId < 0 || ownerId >= static_cast<qint32>(m_relations.size()))
        {
            return Relation::Neutral;
        }
        return m_relations[static_cast<std::size_t>(ownerId)];
    }

    bool BattlefieldKnowledge::onMap(qint32 x, qint32 y) const
    {
        return x >= 0 && y >= 0 && x < m_width && y < m_height;
    }

    std::size_t BattlefieldKnowledge::tileIndex(qint32 x, qint32 y) const
    {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
    }
}
