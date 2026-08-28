#include "tests/arena/propertyfactsprobe.h"

#include <cstddef>
#include <vector>
#include <QStringList>
#include "ai/coreai.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/propertyeconomicsbuilder.h"
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
constexpr bool SKIP_CAPTURE_VISUALS = false;
constexpr qint32 CAPTURE_POINTS = 10;

const Coordinator::PropertyFacts* propertyAt(const std::vector<Coordinator::PropertyFacts> & facts, qint32 x, qint32 y)
{
    for (const Coordinator::PropertyFacts & property : facts)
    {
        if (property.x == x && property.y == y)
            return &property;
    }
    return nullptr;
}

qint32 repairMask(Building & building)
{
    qint32 mask = 0;
    for (const qint32 type : building.getRepairTypes())
        mask |= type;
    return mask;
}

QStringList productionList(Player & observer, Building & building)
{
    const QStringList listed = building.getConstructionList();
    if (building.getOwner() != nullptr)
        return listed;
    const QStringList allowed = observer.getBuildList();
    QStringList filtered;
    for (const QString & unitId : listed)
    {
        if (allowed.contains(unitId))
            filtered.append(unitId);
    }
    return filtered;
}

QString checkClearFacts(GameMap & map, Player & observer, const Coordinator::BattlefieldKnowledge & knowledge,
                        const std::vector<Coordinator::PropertyFacts> & facts)
{
    if (facts.size() != knowledge.buildings().size())
        return QStringLiteral("fact-count-mismatch");
    std::vector<qint64> income(static_cast<std::size_t>(map.getPlayerCount()), 0);
    for (std::size_t i = 0; i < facts.size(); ++i)
    {
        const Coordinator::PropertyFacts & property = facts[i];
        const Coordinator::KnownBuilding & known = knowledge.buildings()[i];
        Terrain* pTerrain = map.getTerrain(property.x, property.y);
        Building* pBuilding = pTerrain != nullptr ? pTerrain->getBuilding() : nullptr;
        if (pBuilding == nullptr || property.buildingIndex != static_cast<qint32>(i) ||
            property.x != known.x || property.y != known.y || property.buildingId != known.buildingId ||
            property.ownerId != known.ownerId)
            return QStringLiteral("identity-mismatch");
        const qint32 covered = pBuilding->getBuildingWidth() * pBuilding->getBuildingHeigth();
        const qint32 paid = pBuilding->getOwner() != nullptr ? pBuilding->getIncome() * covered : 0;
        const bool produces = pBuilding->isProductionBuilding();
        if (!property.visible || property.baseIncome != static_cast<qint32>(pBuilding->getBaseIncome()) ||
            property.coveredTileCount != covered || property.incomePerTurn != paid ||
            property.capturable != pBuilding->isCaptureBuilding() ||
            (property.objective == Coordinator::ObjectiveKind::Headquarters) != pBuilding->isHq() ||
            property.canProduce != produces || property.repairTypeMask != repairMask(*pBuilding) ||
            (produces && property.productionList != productionList(observer, *pBuilding)) ||
            (!produces && !property.productionList.isEmpty()))
            return QStringLiteral("building-facts-mismatch");
        if (property.ownerId >= 0 && property.ownerId < static_cast<qint32>(income.size()))
            income[static_cast<std::size_t>(property.ownerId)] += property.incomePerTurn;
    }
    for (qint32 playerId = 0; playerId < map.getPlayerCount(); ++playerId)
    {
        Player* pPlayer = map.getPlayer(playerId);
        if (pPlayer == nullptr || income[static_cast<std::size_t>(playerId)] != pPlayer->calcIncome())
            return QStringLiteral("income-mismatch");
    }
    return QString();
}

QString checkCapture(const Coordinator::BattlefieldKnowledge & knowledge,
                     const Coordinator::PropertyFacts & property, Unit & capturer)
{
    if (property.capturerIndex < 0 ||
        property.capturerIndex >= static_cast<qint32>(knowledge.units().size()))
        return QStringLiteral("capturer-index-mismatch");
    const Coordinator::KnownUnit & known = knowledge.units()[static_cast<std::size_t>(property.capturerIndex)];
    const qint32 rate = capturer.getCaptureRate(QPoint(property.x, property.y));
    if (known.unitId != QString(CoreAI::UNIT_INFANTRY) || known.ownerId != OBSERVER_PLAYER ||
        property.capturePoints != CAPTURE_POINTS || property.captureRate != rate ||
        property.captureTurnsRemaining !=
            Coordinator::captureTurnsFor(CAPTURE_POINTS, rate, Unit::MAX_CAPTURE_POINTS))
        return QStringLiteral("capture-facts-mismatch");
    return QString();
}

QString propertyEconomics(const QString & mapPath)
{
    spGameMap pMap = MemoryManagement::create<GameMap>(mapPath, MAP_ONLY_LOAD, MAP_FAST_LOAD, MAP_IS_SAVEGAME);
    GameRules* pRules = pMap->getGameRules();
    Player* pObserver = pMap->getPlayer(OBSERVER_PLAYER);
    Player* pEnemy = pMap->getPlayer(ENEMY_PLAYER);
    if (pRules == nullptr || pObserver == nullptr || pEnemy == nullptr || !pObserver->isEnemy(pEnemy))
        return QStringLiteral("invalid-map");
    pRules->setFogMode(GameEnums::Fog_Off);
    Coordinator::BattlefieldKnowledge knowledge = Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    std::vector<Coordinator::PropertyFacts> facts = Coordinator::buildPropertyEconomics(*pMap, knowledge);
    const QString clearFailure = checkClearFacts(*pMap, *pObserver, knowledge, facts);
    if (!clearFailure.isEmpty())
        return clearFailure;
    const Coordinator::PropertyFacts* pTarget = nullptr;
    for (const Coordinator::PropertyFacts & property : facts)
    {
        Terrain* pTerrain = pMap->getTerrain(property.x, property.y);
        if (property.ownerId == ENEMY_PLAYER && property.capturable && pTerrain->getUnit() == nullptr)
        {
            pTarget = &property;
            break;
        }
    }
    if (pTarget == nullptr)
        return QStringLiteral("no-capture-target");
    const qint32 captureX = pTarget->x;
    const qint32 captureY = pTarget->y;
    Unit* pCapturer = pMap->spawnUnit(captureX, captureY, QString(CoreAI::UNIT_INFANTRY), pObserver,
                                      SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    if (pCapturer == nullptr)
        return QStringLiteral("capture-spawn-failed");
    pCapturer->setCapturePoints(CAPTURE_POINTS, SKIP_CAPTURE_VISUALS);
    knowledge = Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    facts = Coordinator::buildPropertyEconomics(*pMap, knowledge);
    const Coordinator::PropertyFacts* pCaptured = propertyAt(facts, captureX, captureY);
    const QString captureFailure = pCaptured != nullptr ? checkCapture(knowledge, *pCaptured, *pCapturer)
                                                        : QStringLiteral("captured-property-missing");
    if (!captureFailure.isEmpty())
        return captureFailure;
    pRules->setFogMode(GameEnums::Fog_OfWar);
    pObserver->updatePlayerVision(false, true);
    const Coordinator::PropertyFacts* pHidden = nullptr;
    for (const Coordinator::PropertyFacts & property : facts)
    {
        Terrain* pTerrain = pMap->getTerrain(property.x, property.y);
        if (property.ownerId == Coordinator::NO_OWNER && property.capturable &&
            !pObserver->getFieldVisible(property.x, property.y) && pTerrain->getUnit() == nullptr)
        {
            pHidden = &property;
            break;
        }
    }
    if (pHidden == nullptr)
        return QStringLiteral("no-hidden-neutral-property");
    const qint32 hiddenX = pHidden->x;
    const qint32 hiddenY = pHidden->y;
    Unit* pHiddenCapturer = pMap->spawnUnit(hiddenX, hiddenY, QString(CoreAI::UNIT_INFANTRY), pEnemy,
                                            SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    if (pHiddenCapturer == nullptr)
        return QStringLiteral("hidden-capture-spawn-failed");
    pHiddenCapturer->setCapturePoints(CAPTURE_POINTS, SKIP_CAPTURE_VISUALS);
    pRules->setFogMode(GameEnums::Fog_Off);
    const Coordinator::BattlefieldKnowledge seen = Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    const std::vector<Coordinator::PropertyFacts> seenFacts = Coordinator::buildPropertyEconomics(*pMap, seen);
    const Coordinator::PropertyFacts* pSeen = propertyAt(seenFacts, hiddenX, hiddenY);
    if (pSeen == nullptr || !pSeen->visible || pSeen->capturerIndex == Coordinator::NO_CAPTURER ||
        pSeen->capturePoints != CAPTURE_POINTS)
        return QStringLiteral("visible-capture-missing");
    pRules->setFogMode(GameEnums::Fog_OfWar);
    pObserver->updatePlayerVision(false, true);
    const Coordinator::BattlefieldKnowledge fogged = Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    const std::vector<Coordinator::PropertyFacts> foggedFacts =
        Coordinator::buildPropertyEconomics(*pMap, fogged);
    const Coordinator::PropertyFacts* pFogged = propertyAt(foggedFacts, hiddenX, hiddenY);
    if (pFogged == nullptr || pFogged->visible || pFogged->capturerIndex != Coordinator::NO_CAPTURER ||
        pFogged->capturePoints != 0 || pFogged->captureRate != 0 ||
        pFogged->captureTurnsRemaining != Coordinator::NO_CAPTURE_TURNS)
        return QStringLiteral("fogged-capture-leaked");
    return QString();
}
}

QVariantMap PropertyFactsProbe::run(QObject*, const QVariantMap & arguments)
{
    const QString mapPath = arguments.value(QStringLiteral("mapPath")).toString();
    const QString failure = mapPath.isEmpty() ? QStringLiteral("missing-map-path") : propertyEconomics(mapPath);
    return {
        {QStringLiteral("ok"), failure.isEmpty()},
        {QStringLiteral("failures"), failure.isEmpty() ? QStringList()
                                                       : QStringList{mapPath + QStringLiteral(":") + failure}},
    };
}

namespace
{
[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("propertyEconomicsFacts"), PropertyFactsProbe::run);
}
