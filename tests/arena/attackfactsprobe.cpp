#include "tests/arena/attackfactsprobe.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <QStringList>
#include "ai/coreai.h"
#include "ai/coordinator/attackopportunitybuilder.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "coreengine/memorymanagement.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/unitpathfindingsystem.h"
#include "game/weaponrangecheck.h"
#include "resource_management/unitspritemanager.h"
#include "tests/arena/arenatestsupport.h"
namespace
{
constexpr bool MAP_ONLY_LOAD = true;
constexpr bool MAP_FAST_LOAD = false;
constexpr bool MAP_IS_SAVEGAME = false;
constexpr qint32 OBSERVER_PLAYER = 0;
constexpr qint32 ENEMY_PLAYER = 1;
constexpr quint8 CO_SLOT_PRIMARY = 0;
constexpr qint32 SPAWN_RANGE_EXACT = 0;
constexpr bool SPAWN_IGNORE_MOVEMENT = true;
const QString MOVEMENT_CO = QStringLiteral("CO_STURM");
const QString TRANSPORT_UNIT_ID = QStringLiteral("APC");
QString describeTile(qint32 x, qint32 y)
{
    return QString::number(x) + QStringLiteral(",") + QString::number(y);
}
bool findFreeTile(GameMap & map, qint32 & outX, qint32 & outY)
{
    for (qint32 y = map.getMapHeight() - 1; y >= 0; --y)
    {
        for (qint32 x = map.getMapWidth() - 1; x >= 0; --x)
        {
            if (map.getTerrain(x, y)->getUnit() == nullptr)
            {
                outX = x;
                outY = y;
                return true;
            }
        }
    }
    return false;
}

qint32 attackerSlotForUnit(const Coordinator::BattlefieldKnowledge & knowledge, const Coordinator::AttackOpportunityField & field,
                           const Coordinator::KnownUnit & known)
{
    const qint32 unitIndex = static_cast<qint32>(&known - knowledge.units().data());
    for (qint32 slot = 0; slot < field.attackerCount(); ++slot)
    {
        if (field.attacker(slot).unitIndex == unitIndex)
        {
            return slot;
        }
    }
    return -1;
}
QString differentialForUnit(GameMap & map, Player & observer, Unit & unit, const Coordinator::BattlefieldKnowledge & knowledge,
                            const Coordinator::AttackOpportunityField & field,
                            qint32 & expectedAttackers)
{
    const qint32 x = unit.Unit::getX();
    const qint32 y = unit.Unit::getY();
    const QString label = unit.getUnitID() + QStringLiteral(" at ") + describeTile(x, y);
    const Coordinator::KnownUnit* pKnown = knowledge.unitAt(x, y);
    if (unit.isStealthed(&observer))
    {
        return pKnown == nullptr ? QString() : label + QStringLiteral(" stealthed but known");
    }
    if (pKnown == nullptr)
    {
        return label + QStringLiteral(" missing from knowledge");
    }
    const qint32 slot = attackerSlotForUnit(knowledge, field, *pKnown);
    const WeaponRangeCheck::Slot first{!unit.getWeapon1ID().isEmpty(), unit.hasAmmo1()};
    const WeaponRangeCheck::Slot second{!unit.getWeapon2ID().isEmpty(), unit.hasAmmo2()};
    if (!first.isUsable() && !second.isUsable())
    {
        return slot < 0 ? QString() : label + QStringLiteral(" unarmed but listed");
    }
    if (slot < 0)
    {
        return label + QStringLiteral(" armed but missing");
    }
    ++expectedAttackers;
    const QPoint position(x, y);
    const qint32 movementPoints = unit.getMovementpoints(position);
    const qint32 minRange = unit.getMinRange(position);
    const qint32 maxRange = unit.getMaxRange(position);
    std::vector<QPoint> origins;
    if (unit.canMoveAndFire(position))
    {
        UnitPathFindingSystem pfs(&map, &unit);
        pfs.setIgnoreEnemies(UnitPathFindingSystem::CollisionIgnore::All);
        pfs.setMovepoints(movementPoints);
        pfs.explore();
        origins = pfs.getAllNodePointsFast(movementPoints + 1);
    }
    else
    {
        origins.push_back(position);
    }
    const qint32 width = map.getMapWidth();
    const qint32 height = map.getMapHeight();
    const std::size_t tileCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<bool> engineOrigins(tileCount, false);
    std::vector<bool> engineTargets(tileCount, false);
    for (const QPoint & origin : origins)
    {
        engineOrigins[static_cast<std::size_t>(origin.y() * width + origin.x())] = true;
        for (qint32 dy = -maxRange; dy <= maxRange; ++dy)
        {
            for (qint32 dx = -maxRange; dx <= maxRange; ++dx)
            {
                const qint32 distance = qAbs(dx) + qAbs(dy);
                const qint32 targetX = origin.x() + dx;
                const qint32 targetY = origin.y() + dy;
                if (distance >= minRange && distance <= maxRange && map.onMap(targetX, targetY))
                {
                    engineTargets[static_cast<std::size_t>(targetY * width + targetX)] = true;
                }
            }
        }
    }
    for (qint32 targetY = 0; targetY < height; ++targetY)
    {
        for (qint32 targetX = 0; targetX < width; ++targetX)
        {
            const std::size_t tile = static_cast<std::size_t>(targetY * width + targetX);
            if (field.canOriginateFrom(slot, targetX, targetY) != engineOrigins[tile])
            {
                return label + QStringLiteral(" origin ") + describeTile(targetX, targetY);
            }
            if (field.canAttack(slot, targetX, targetY) != engineTargets[tile])
            {
                return label + QStringLiteral(" target ") + describeTile(targetX, targetY);
            }
        }
    }
    return QString();
}

QString attackOpportunityDifferential(const QString & mapPath)
{
    spGameMap pMap = MemoryManagement::create<GameMap>(mapPath, MAP_ONLY_LOAD, MAP_FAST_LOAD, MAP_IS_SAVEGAME);
    GameRules* pRules = pMap->getGameRules();
    if (pRules == nullptr || pMap->getPlayerCount() <= ENEMY_PLAYER)
    {
        return QStringLiteral("invalid-map");
    }
    Player* pObserver = pMap->getPlayer(OBSERVER_PLAYER);
    Player* pEnemy = pMap->getPlayer(ENEMY_PLAYER);
    if (pObserver == nullptr || pEnemy == nullptr || !pObserver->isEnemy(pEnemy))
    {
        return QStringLiteral("invalid-players");
    }
    pEnemy->setCO(MOVEMENT_CO, CO_SLOT_PRIMARY);
    if (pEnemy->getCO(CO_SLOT_PRIMARY) == nullptr)
    {
        return QStringLiteral("co-not-loaded:") + MOVEMENT_CO;
    }
    pRules->setFogMode(GameEnums::Fog_Off);
    const QStringList roster = UnitSpriteManager::getInstance()->getLoadedRessources();
    if (roster.isEmpty())
    {
        return QStringLiteral("no-unit-roster");
    }
    Unit* pEnemyTransport = nullptr;
    for (const QString & unitId : roster)
    {
        qint32 x = -1;
        qint32 y = -1;
        if (!findFreeTile(*pMap, x, y))
        {
            return QStringLiteral("roster-exceeds-map");
        }
        Unit* pSpawned = pMap->spawnUnit(x, y, unitId, pEnemy, SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
        if (pSpawned == nullptr)
        {
            return QStringLiteral("roster-spawn-failed:") + unitId;
        }
        if (unitId == TRANSPORT_UNIT_ID)
        {
            pEnemyTransport = pSpawned;
        }
    }
    if (pEnemyTransport == nullptr)
    {
        return QStringLiteral("transport-not-in-roster");
    }
    pEnemyTransport->loadSpawnedUnit(QString(CoreAI::UNIT_INFANTRY));
    if (pEnemyTransport->getLoadedUnitCount() != 1)
    {
        return QStringLiteral("cargo-load-failed");
    }
    const Coordinator::BattlefieldKnowledge knowledge = Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    Coordinator::MobilityFieldCache cache;
    const Coordinator::AttackOpportunityField field = Coordinator::buildAttackOpportunityField(*pMap, knowledge, cache);
    qint32 expectedAttackers = 0;
    for (qint32 y = 0; y < pMap->getMapHeight(); ++y)
    {
        for (qint32 x = 0; x < pMap->getMapWidth(); ++x)
        {
            Unit* pUnit = pMap->getTerrain(x, y)->getUnit();
            if (pUnit != nullptr && pUnit->getOwner() == pEnemy)
            {
                const QString failure = differentialForUnit(*pMap, *pObserver, *pUnit, knowledge, field, expectedAttackers);
                if (!failure.isEmpty())
                {
                    return failure;
                }
            }
        }
    }
    if (field.attackerCount() != expectedAttackers)
    {
        return QStringLiteral("attacker-count mismatch");
    }
    for (qint32 slot = 0; slot < field.attackerCount(); ++slot)
    {
        const qint32 index = field.attacker(slot).unitIndex;
        if (knowledge.units()[static_cast<std::size_t>(index)].carrier != Coordinator::KnownUnit::NO_CARRIER)
        {
            return QStringLiteral("cargo-listed-as-attacker");
        }
    }
    for (qint32 y = 0; y < pMap->getMapHeight(); ++y)
    {
        for (qint32 x = 0; x < pMap->getMapWidth(); ++x)
        {
            qint32 expected = 0;
            for (qint32 slot = 0; slot < field.attackerCount(); ++slot)
            {
                expected += field.canAttack(slot, x, y) ? 1 : 0;
            }
            const std::span<const std::int32_t> listed = field.attackerSlotsAt(x, y);
            bool consistent = static_cast<qint32>(listed.size()) == expected && field.threatened(x, y) == (expected > 0);
            for (const std::int32_t slot : listed)
            {
                consistent = consistent && field.canAttack(slot, x, y);
            }
            if (!consistent)
            {
                return QStringLiteral("tile-list-mismatch ") + describeTile(x, y);
            }
        }
    }
    return QString();
}
}

QVariantMap AttackFactsProbe::run(QObject*, const QVariantMap & arguments)
{
    const QString mapPath = arguments.value(QStringLiteral("mapPath")).toString();
    const QString failure = mapPath.isEmpty() ? QStringLiteral("missing-map-path") : attackOpportunityDifferential(mapPath);
    return {
        {QStringLiteral("ok"), failure.isEmpty()},
        {QStringLiteral("failures"), failure.isEmpty() ? QStringList() : QStringList{mapPath + QStringLiteral(":") + failure}},
    };
}
namespace
{
[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("attackOpportunityDifferential"), AttackFactsProbe::run);
}
