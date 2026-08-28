#include "tests/arena/mobilityprobe.h"

#include <QStringList>

#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "coreengine/memorymanagement.h"
#include "game/co.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/unit.h"
#include "game/unitpathfindingsystem.h"
#include "resource_management/unitspritemanager.h"
#include "tests/arena/arenatestsupport.h"

namespace
{
constexpr bool MAP_ONLY_LOAD = true;
constexpr bool MAP_FAST_LOAD = false;
constexpr bool MAP_IS_SAVEGAME = false;
constexpr quint8 CO_SLOT_PRIMARY = 0;
constexpr qint32 UNLIMITED_MOVEPOINTS = -2;
constexpr qint32 REVERSAL_SAMPLE_STRIDE = 7;
const QString DIFFERENTIAL_CO_PLAYER_0 = QStringLiteral("CO_STURM");
const QString DIFFERENTIAL_CO_PLAYER_1 = QStringLiteral("CO_SAMI");

QString describeTile(qint32 x, qint32 y)
{
    return QString::number(x) + QStringLiteral(",") + QString::number(y);
}

QString mobilityFieldDifferentialForUnit(GameMap & map, Player & owner, const QString & unitId,
                                         Coordinator::MobilityFieldCache & cache)
{
    const qint32 width = map.getMapWidth();
    const qint32 height = map.getMapHeight();
    const Coordinator::MobilityCostGrid & grid = cache.grid(map, owner, unitId);
    qint32 startX = -1;
    qint32 startY = -1;
    for (qint32 y = 0; y < height && startX < 0; ++y)
    {
        for (qint32 x = 0; x < width && startX < 0; ++x)
        {
            if (map.getTerrain(x, y)->getUnit() != nullptr)
            {
                continue;
            }
            for (const Coordinator::EntryDirection direction : Coordinator::ENTRY_DIRECTIONS)
            {
                if (grid.cost(x, y, direction) >= 0)
                {
                    startX = x;
                    startY = y;
                    break;
                }
            }
        }
    }
    const QString label = unitId + QStringLiteral(" owner ") + QString::number(owner.getPlayerID());
    if (startX < 0)
    {
        return QString();
    }
    Unit* pUnit = map.spawnUnit(startX, startY, unitId, &owner, 0, true);
    if (pUnit == nullptr)
    {
        return label + QStringLiteral(" spawn failed at ") + describeTile(startX, startY);
    }
    const Coordinator::DistanceField field =
        Coordinator::DistanceField::build(grid, {{startX, startY}}, Coordinator::FieldExpansion::FromSources);
    UnitPathFindingSystem pfs(&map, pUnit);
    pfs.setIgnoreEnemies(UnitPathFindingSystem::CollisionIgnore::All);
    pfs.setMovepoints(UNLIMITED_MOVEPOINTS);
    pfs.explore();
    QString failure;
    qint32 reachableCount = 0;
    for (qint32 ty = 0; ty < height && failure.isEmpty(); ++ty)
    {
        for (qint32 tx = 0; tx < width && failure.isEmpty(); ++tx)
        {
            const qint32 engineCost = pfs.getTargetCosts(tx, ty);
            const qint32 fieldCost = field.distance(tx, ty);
            if (engineCost != fieldCost)
            {
                failure = label + QStringLiteral(" from ") + describeTile(startX, startY) +
                          QStringLiteral(" tile ") + describeTile(tx, ty) +
                          QStringLiteral(" engine=") + QString::number(engineCost) +
                          QStringLiteral(" field=") + QString::number(fieldCost);
                break;
            }
            if (engineCost < 0)
            {
                continue;
            }
            ++reachableCount;
            if (reachableCount % REVERSAL_SAMPLE_STRIDE != 0)
            {
                continue;
            }
            const Coordinator::DistanceField backward =
                Coordinator::DistanceField::build(grid, {{tx, ty}}, Coordinator::FieldExpansion::ToTargets);
            if (backward.distance(startX, startY) != engineCost)
            {
                failure = label + QStringLiteral(" reversal ") + describeTile(tx, ty) +
                          QStringLiteral(" engine=") + QString::number(engineCost) +
                          QStringLiteral(" backward=") + QString::number(backward.distance(startX, startY));
            }
        }
    }
    map.getTerrain(startX, startY)->setUnit(spUnit());
    return failure;
}

QString mobilityFieldDifferential(const QString & mapPath)
{
    spGameMap pMap = MemoryManagement::create<GameMap>(
        mapPath, MAP_ONLY_LOAD, MAP_FAST_LOAD, MAP_IS_SAVEGAME);
    if (pMap->getPlayerCount() <= 0)
    {
        return QStringLiteral("no-players");
    }
    Player* pFirstPlayer = pMap->getPlayer(0);
    pFirstPlayer->setCO(DIFFERENTIAL_CO_PLAYER_0, CO_SLOT_PRIMARY);
    if (pMap->getPlayerCount() > 1)
    {
        pMap->getPlayer(1)->setCO(DIFFERENTIAL_CO_PLAYER_1, CO_SLOT_PRIMARY);
    }
    if (pFirstPlayer->getCO(CO_SLOT_PRIMARY) == nullptr)
    {
        return QStringLiteral("co-not-loaded:") + DIFFERENTIAL_CO_PLAYER_0;
    }
    const QStringList roster = UnitSpriteManager::getInstance()->getLoadedRessources();
    if (roster.isEmpty())
    {
        return QStringLiteral("no-unit-roster");
    }
    Coordinator::MobilityFieldCache cache;
    for (qint32 player = 0; player < pMap->getPlayerCount(); ++player)
    {
        Player* pOwner = pMap->getPlayer(player);
        for (const QString & unitId : roster)
        {
            const QString failure = mobilityFieldDifferentialForUnit(*pMap, *pOwner, unitId, cache);
            if (!failure.isEmpty())
            {
                return failure;
            }
        }
    }
    if (cache.epochChangeCount() != 0)
    {
        return QStringLiteral("cache-epoch-changed-during-sweep");
    }
    pMap->bumpTerrainGeneration();
    cache.grid(*pMap, *pFirstPlayer, roster.first());
    if (cache.epochChangeCount() != 1 || cache.profileCount() != 1)
    {
        return QStringLiteral("cache-ignored-terrain-generation");
    }
    pFirstPlayer->getCO(CO_SLOT_PRIMARY)->setPowerMode(GameEnums::PowerMode_Power);
    cache.grid(*pMap, *pFirstPlayer, roster.first());
    if (cache.epochChangeCount() != 2 || cache.profileCount() != 1)
    {
        return QStringLiteral("cache-ignored-power-mode");
    }
    return QString();
}
}

QVariantMap MobilityProbe::run(QObject*, const QVariantMap & arguments)
{
    QStringList failures;
    const QString mapPath = arguments.value(QStringLiteral("mapPath")).toString();
    if (mapPath.isEmpty())
    {
        failures.append(QStringLiteral("missing-map-path"));
    }
    else
    {
        const QString failure = mobilityFieldDifferential(mapPath);
        if (!failure.isEmpty())
        {
            failures.append(mapPath + QStringLiteral(":") + failure);
        }
    }
    return {
        {QStringLiteral("ok"), failures.isEmpty()},
        {QStringLiteral("failures"), failures},
    };
}

namespace
{
[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("mobilityFieldDifferential"), MobilityProbe::run);
}
