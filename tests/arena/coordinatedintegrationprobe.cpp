#include "tests/arena/coordinatedintegrationprobe.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QIODevice>
#include <QPoint>
#include <QStringList>

#include <vector>

#include "ai/coordinatedai.h"
#include "ai/coreai.h"
#include "ai/normalai.h"
#include "coreengine/globalutils.h"
#include "coreengine/memorymanagement.h"
#include "game/building.h"
#include "game/gameaction.h"
#include "game/gameanimation/gameanimationfactory.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "gameinput/basegameinputif.h"
#include "tests/arena/arenatestsupport.h"

namespace
{
constexpr bool MAP_ONLY_LOAD = true;
constexpr bool MAP_FAST_LOAD = false;
constexpr bool MAP_IS_SAVEGAME = false;
constexpr qint32 ACTOR_PLAYER = 0;
constexpr qint32 ENEMY_PLAYER = 1;
constexpr qint32 START_DAY = 1;
constexpr qint32 SPAWN_RANGE_EXACT = 0;
constexpr bool SPAWN_CHECK_MOVEMENT = false;
constexpr bool SKIP_CAPTURE_VISUALS = false;
constexpr qint32 CAPTURE_POINTS = 10;
constexpr qint32 PAUSE_OBSERVATION_MS = 1250;
constexpr quint32 MASTER_SEED = 20260826u;
const QString TOWN_ID = QStringLiteral("TOWN");
const QString COORDINATED_CONTROLLER =
    QStringLiteral("Coordinated");
const QString NORMAL_CONTROLLER = QStringLiteral("Normal");

struct Checks
{
    void expect(bool condition, const QString & failure)
    {
        if (!condition)
        {
            failures.append(failure);
        }
    }

    QStringList failures;
};

struct Fixture
{
    spGameMap map;
    spBaseGameInputIF input;
    CoreAI* ai{nullptr};
    Player* actorPlayer{nullptr};
    Unit* capturer{nullptr};
    Building* town{nullptr};
    QPoint anchor;
};

QString hashMap(GameMap & map)
{
    return QString::fromLatin1(map.getMapHash().toHex());
}

QString hashPayload(const QByteArray & payload)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex());
}

QByteArray serializeAction(
    const spGameAction & action, Checks & checks)
{
    QByteArray payload;
    if (action == nullptr)
    {
        checks.expect(false, QStringLiteral("null action"));
        return payload;
    }
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    action->serializeObject(stream);
    checks.expect(
        stream.status() == QDataStream::Ok,
        QStringLiteral("action serialization failed"));
    return payload;
}

bool addCaptureAnchor(Fixture & fixture, Checks & checks)
{
    for (qint32 y = 0;
         y < fixture.map->getMapHeight();
         ++y)
    {
        for (qint32 x = 0;
             x < fixture.map->getMapWidth();
             ++x)
        {
            Terrain* pTerrain = fixture.map->getTerrain(x, y);
            if (pTerrain == nullptr ||
                pTerrain->getUnit() != nullptr ||
                pTerrain->getBuilding() != nullptr)
            {
                continue;
            }
            Unit* pUnit = fixture.map->spawnUnit(
                x,
                y,
                QString::fromLatin1(CoreAI::UNIT_INFANTRY),
                fixture.actorPlayer,
                SPAWN_RANGE_EXACT,
                SPAWN_CHECK_MOVEMENT);
            if (pUnit == nullptr)
            {
                continue;
            }
            spBuilding pTown =
                MemoryManagement::create<Building>(
                    TOWN_ID, fixture.map.get());
            pTown->setOwner(nullptr);
            pTerrain->setBuilding(pTown);
            pUnit->setCapturePoints(
                CAPTURE_POINTS, SKIP_CAPTURE_VISUALS);
            pUnit->setHasMoved(false);
            fixture.capturer = pUnit;
            fixture.town = pTown.get();
            fixture.anchor = QPoint(x, y);
            return true;
        }
    }
    checks.expect(
        false, QStringLiteral("capture anchor unavailable"));
    return false;
}

GameEnums::AiTypes controllerType(
    const QString & controller, Checks & checks)
{
    if (controller == COORDINATED_CONTROLLER)
    {
        return GameEnums::AiTypes_Coordinated;
    }
    if (controller == NORMAL_CONTROLLER)
    {
        return GameEnums::AiTypes_Normal;
    }
    checks.expect(false, QStringLiteral("unknown controller"));
    return GameEnums::AiTypes_Closed;
}

Fixture makeFixture(
    const QString & mapPath,
    const QString & controller,
    Checks & checks)
{
    Fixture fixture;
    fixture.map = MemoryManagement::create<GameMap>(
        mapPath,
        MAP_ONLY_LOAD,
        MAP_FAST_LOAD,
        MAP_IS_SAVEGAME);
    checks.expect(
        fixture.map != nullptr,
        QStringLiteral("map load failed"));
    if (fixture.map == nullptr)
    {
        return fixture;
    }
    fixture.actorPlayer =
        fixture.map->getPlayer(ACTOR_PLAYER);
    Player* pEnemy =
        fixture.map->getPlayer(ENEMY_PLAYER);
    checks.expect(
        fixture.actorPlayer != nullptr,
        QStringLiteral("missing actor player"));
    checks.expect(
        pEnemy != nullptr,
        QStringLiteral("missing enemy player"));
    if (fixture.actorPlayer == nullptr || pEnemy == nullptr)
    {
        return fixture;
    }
    fixture.actorPlayer->setTeam(ACTOR_PLAYER);
    pEnemy->setTeam(ENEMY_PLAYER);
    fixture.map->setCurrentDay(START_DAY);
    fixture.map->setCurrentPlayer(ACTOR_PLAYER);
    fixture.map->getGameRules()->setFogMode(
        GameEnums::Fog_Off);
    fixture.map->getGameRules()->setAiBehaviorMode(
        GameEnums::AiBehavior_Standard);
    if (!addCaptureAnchor(fixture, checks))
    {
        return fixture;
    }
    checks.expect(
        fixture.actorPlayer->getUnitCount() == 1,
        QStringLiteral("unexpected existing actor units"));
    const GameEnums::AiTypes aiType =
        controllerType(controller, checks);
    if (!checks.failures.isEmpty())
    {
        return fixture;
    }
    fixture.actorPlayer->setControlType(aiType);
    fixture.input =
        BaseGameInputIF::createAi(fixture.map.get(), aiType);
    fixture.actorPlayer->setBaseGameInput(fixture.input);
    fixture.ai = dynamic_cast<CoreAI*>(fixture.input.get());
    checks.expect(
        fixture.ai != nullptr,
        QStringLiteral("controller construction failed"));
    if (fixture.ai == nullptr)
    {
        return fixture;
    }
    NormalAi* pNormalAi =
        dynamic_cast<NormalAi*>(fixture.ai);
    checks.expect(
        pNormalAi != nullptr,
        QStringLiteral("normal controller base unavailable"));
    if (pNormalAi == nullptr)
    {
        return fixture;
    }
    fixture.ai->resetMoveMap();
    fixture.ai->onGameStart();
    QObject::connect(
        fixture.map.get(),
        &GameMap::sigToggleAiPause,
        pNormalAi,
        &NormalAi::toggleAiPause,
        Qt::DirectConnection);
    return fixture;
}

void observePauseWindow()
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < PAUSE_OBSERVATION_MS)
    {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents, 25);
    }
}

void processOnce(CoreAI & ai)
{
    GameAction::pinActionSeeds(MASTER_SEED);
    ai.process();
    GameAction::unpinActionSeeds();
}

void performAction(
    const spGameAction & action, Checks & checks)
{
    if (action == nullptr)
    {
        return;
    }
    action->perform();
    GameAnimationFactory::finishAllAnimations();
    checks.expect(
        GameAnimationFactory::getAnimationCount() == 0,
        QStringLiteral("action animations remain"));
}

QVariantMap runFixture(
    const QString & mapPath,
    const QString & controller,
    bool pause,
    Checks & checks)
{
    GameAnimationFactory::clearAllAnimations();
    Fixture fixture =
        makeFixture(mapPath, controller, checks);
    std::vector<spGameAction> actions;
    if (fixture.ai == nullptr ||
        fixture.capturer == nullptr ||
        fixture.town == nullptr)
    {
        return {};
    }
    const QMetaObject::Connection actionConnection =
        QObject::connect(
            fixture.ai,
            &CoreAI::sigPerformAction,
            fixture.ai,
            [&actions](spGameAction action, bool)
            {
                actions.push_back(std::move(action));
            },
            Qt::DirectConnection);

    const QString preStateHash = hashMap(*fixture.map);
    if (pause)
    {
        fixture.map->toggleAiPause();
        processOnce(*fixture.ai);
        observePauseWindow();
        checks.expect(
            actions.empty(),
            QStringLiteral("paused controller emitted an action"));
        checks.expect(
            hashMap(*fixture.map) == preStateHash,
            QStringLiteral("paused map state changed"));
        fixture.map->toggleAiPause();
    }
    processOnce(*fixture.ai);
    QObject::disconnect(actionConnection);
    checks.expect(
        actions.size() == 1,
        QStringLiteral("expected one emitted action"));

    spGameAction action =
        actions.size() == 1 ? actions.front() : spGameAction();
    const QByteArray payload =
        serializeAction(action, checks);
    if (action != nullptr)
    {
        checks.expect(
            action->getActionID() ==
                QString::fromLatin1(CoreAI::ACTION_CAPTURE),
            QStringLiteral("emitted action is not capture"));
        checks.expect(
            action->getActionTarget() == fixture.anchor,
            QStringLiteral("capture target changed"));
    }
    performAction(action, checks);
    const QString finalStateHash = hashMap(*fixture.map);
    checks.expect(
        finalStateHash != preStateHash,
        QStringLiteral("post-action state did not change"));
    checks.expect(
        fixture.town->getOwner() == fixture.actorPlayer,
        QStringLiteral("town was not captured"));
    checks.expect(
        fixture.capturer->getCapturePoints() == 0,
        QStringLiteral("capture points were not consumed"));
    checks.expect(
        fixture.capturer->getHasMoved(),
        QStringLiteral("capturer was not marked moved"));
    GameAnimationFactory::clearAllAnimations();

    return {
        {QStringLiteral("actionCount"),
         static_cast<qint32>(actions.size())},
        {QStringLiteral("actionId"),
         action == nullptr ? QString() : action->getActionID()},
        {QStringLiteral("targetX"), fixture.anchor.x()},
        {QStringLiteral("targetY"), fixture.anchor.y()},
        {QStringLiteral("payloadSha256"),
         hashPayload(payload)},
        {QStringLiteral("preStateHash"), preStateHash},
        {QStringLiteral("finalStateHash"), finalStateHash},
    };
}
}

QVariantMap CoordinatedIntegrationProbe::run(
    QObject*, const QVariantMap & arguments)
{
    Checks checks;
    const QString mapPath =
        arguments.value(QStringLiteral("mapPath")).toString();
    const QString controller =
        arguments.value(QStringLiteral("controller")).toString();
    const bool pause =
        arguments.value(QStringLiteral("pause")).toBool();
    checks.expect(
        !mapPath.isEmpty(),
        QStringLiteral("missing map path"));
    GlobalUtils::seed(MASTER_SEED);
    GlobalUtils::setUseSeed(true);
    QVariantMap result;
    if (checks.failures.isEmpty())
    {
        result = runFixture(
            mapPath, controller, pause, checks);
    }
    GameAction::unpinActionSeeds();
    GlobalUtils::setUseSeed(false);
    result.insert(
        QStringLiteral("ok"), checks.failures.isEmpty());
    result.insert(
        QStringLiteral("failures"), checks.failures);
    return result;
}

namespace
{
[[maybe_unused]] const bool REGISTERED =
    AiArenaTestSupport::registerOperation(
        QStringLiteral("coordinatedIntegrationProbe"),
        CoordinatedIntegrationProbe::run);
}
