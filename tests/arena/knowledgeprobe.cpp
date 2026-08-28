#include "tests/arena/knowledgeprobe.h"

#include <QStringList>

#include "ai/coreai.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "coreengine/memorymanagement.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "tests/arena/arenatestsupport.h"

namespace
{
constexpr bool MAP_ONLY_LOAD = true;
constexpr bool MAP_FAST_LOAD = false;
constexpr bool MAP_IS_SAVEGAME = false;
constexpr qint32 OBSERVER_PLAYER = 0;
constexpr qint32 ENEMY_PLAYER = 1;
constexpr qint32 SPAWN_RANGE_EXACT = 0;
constexpr bool SPAWN_IGNORE_MOVEMENT = true;
constexpr qint32 CAPTURE_POINTS = 7;
constexpr qint32 FOG_ROW_LENGTH = 2;

qint32 ownerId(Player* pOwner)
{
    return pOwner != nullptr ? pOwner->getPlayerID() : Coordinator::NO_OWNER;
}

bool tileFree(GameMap & map, qint32 x, qint32 y)
{
    return map.getTerrain(x, y)->getUnit() == nullptr;
}

bool findHiddenFreeTile(GameMap & map, Player & observer, qint32 & outX, qint32 & outY)
{
    for (qint32 y = map.getMapHeight() - 1; y >= 0; --y)
    {
        for (qint32 x = map.getMapWidth() - 1; x >= 0; --x)
        {
            if (tileFree(map, x, y) && !observer.getFieldVisible(x, y))
            {
                outX = x;
                outY = y;
                return true;
            }
        }
    }
    return false;
}

bool findFreeRow(GameMap & map, qint32 length, qint32 & outX, qint32 & outY)
{
    for (qint32 y = map.getMapHeight() - 1; y >= 0; --y)
    {
        for (qint32 x = map.getMapWidth() - length; x >= 0; --x)
        {
            bool free = true;
            for (qint32 offset = 0; offset < length; ++offset)
            {
                free = free && tileFree(map, x + offset, y);
            }
            if (free)
            {
                outX = x;
                outY = y;
                return true;
            }
        }
    }
    return false;
}

bool captureFactsMatch(const Coordinator::KnownUnit & known, Unit & live)
{
    const QPoint position(known.x, known.y);
    return known.unitId == live.getUnitID() &&
           known.ownerId == ownerId(live.getOwner()) &&
           known.capturePoints == live.getCapturePoints() &&
           known.canCapture == live.canCapture() &&
           known.captureRate == live.getCaptureRate(position);
}

QString knowledgeFacts(const QString & mapPath)
{
    spGameMap pMap = MemoryManagement::create<GameMap>(
        mapPath, MAP_ONLY_LOAD, MAP_FAST_LOAD, MAP_IS_SAVEGAME);
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
    pRules->setFogMode(GameEnums::Fog_OfWar);
    qint32 rowX = -1;
    qint32 rowY = -1;
    if (!findFreeRow(*pMap, FOG_ROW_LENGTH, rowX, rowY))
    {
        return QStringLiteral("no-free-row");
    }
    const QString infantry = QString(CoreAI::UNIT_INFANTRY);
    const qint32 nearX = rowX;
    const qint32 ownX = rowX + 1;
    Unit* pOwn = pMap->spawnUnit(ownX, rowY, infantry, pObserver, SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    Unit* pNear = pMap->spawnUnit(nearX, rowY, infantry, pEnemy, SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    if (pOwn == nullptr || pNear == nullptr)
    {
        return QStringLiteral("spawn-failed");
    }
    pOwn->setCapturePoints(CAPTURE_POINTS, false);
    pObserver->updatePlayerVision(false, true);
    qint32 farX = -1;
    qint32 farY = -1;
    if (!findHiddenFreeTile(*pMap, *pObserver, farX, farY) ||
        pMap->spawnUnit(farX, farY, infantry, pEnemy, SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT) == nullptr)
    {
        return QStringLiteral("hidden-spawn-failed");
    }
    const Coordinator::BattlefieldKnowledge fogged =
        Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    const Coordinator::KnownUnit* pOwnKnown = fogged.unitAt(ownX, rowY);
    const Coordinator::KnownUnit* pNearKnown = fogged.unitAt(nearX, rowY);
    if (pOwnKnown == nullptr || !captureFactsMatch(*pOwnKnown, *pOwn) ||
        fogged.relation(pOwnKnown->ownerId) != Coordinator::Relation::Own)
    {
        return QStringLiteral("own-facts-mismatch");
    }
    if (pNearKnown == nullptr ||
        fogged.relation(pNearKnown->ownerId) != Coordinator::Relation::Enemy)
    {
        return QStringLiteral("visible-enemy-missing");
    }
    if (fogged.unitAt(farX, farY) != nullptr || fogged.isVisible(farX, farY))
    {
        return QStringLiteral("hidden-enemy-leaked");
    }
    pRules->setFogMode(GameEnums::Fog_Off);
    const Coordinator::BattlefieldKnowledge clear =
        Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    if (clear.unitAt(farX, farY) == nullptr ||
        clear.buildings().size() != static_cast<std::size_t>(pMap->getBuildingCount(QString())))
    {
        return QStringLiteral("clear-facts-mismatch");
    }
    for (const Coordinator::KnownBuilding & known : clear.buildings())
    {
        Terrain* pTerrain = pMap->getTerrain(known.x, known.y);
        Building* pBuilding = pTerrain != nullptr ? pTerrain->getBuilding() : nullptr;
        if (pBuilding == nullptr || pBuilding->getTerrain() != pTerrain ||
            known.buildingId != pBuilding->getBuildingID() ||
            known.ownerId != ownerId(pBuilding->getOwner()))
        {
            return QStringLiteral("building-facts-mismatch");
        }
    }
    pRules->setFogMode(GameEnums::Fog_OfShroud);
    pObserver->loadVisionFields();
    pObserver->updatePlayerVision(false, true);
    const Coordinator::KnownBuilding* pHiddenBuilding = nullptr;
    for (const Coordinator::KnownBuilding & known : clear.buildings())
    {
        if (pObserver->getFieldVisibleType(known.x, known.y) == GameEnums::VisionType_Shrouded)
        {
            pHiddenBuilding = &known;
            break;
        }
    }
    if (pHiddenBuilding == nullptr)
    {
        return QStringLiteral("no-shrouded-building");
    }
    const Coordinator::BattlefieldKnowledge shrouded =
        Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    for (const Coordinator::KnownBuilding & known : shrouded.buildings())
    {
        if (known.x == pHiddenBuilding->x && known.y == pHiddenBuilding->y)
        {
            return QStringLiteral("shrouded-building-leaked");
        }
    }
    if (shrouded.unitAt(ownX, rowY) == nullptr ||
        shrouded.visionType(ownX, rowY) != GameEnums::VisionType_Clear)
    {
        return QStringLiteral("own-unit-missing-under-shroud");
    }
    return QString();
}
}

QVariantMap KnowledgeProbe::run(QObject*, const QVariantMap & arguments)
{
    const QString mapPath = arguments.value(QStringLiteral("mapPath")).toString();
    const QString failure = mapPath.isEmpty()
                                ? QStringLiteral("missing-map-path")
                                : knowledgeFacts(mapPath);
    return {
        {QStringLiteral("ok"), failure.isEmpty()},
        {QStringLiteral("failures"), failure.isEmpty()
                                          ? QStringList()
                                          : QStringList{mapPath + QStringLiteral(":") + failure}},
    };
}

namespace
{
[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("battlefieldKnowledgeFacts"), KnowledgeProbe::run);
}
