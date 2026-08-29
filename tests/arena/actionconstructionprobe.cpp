#include "tests/arena/actionconstructionprobe.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QPoint>
#include <QStringList>
#include <QVector>

#include "ai/coreai.h"
#include "ai/coordinator/engineactionbuilder.h"
#include "coreengine/memorymanagement.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/unit.h"
#include "game/unitpathfindingsystem.h"
#include "tests/arena/arenatestsupport.h"

namespace
{
constexpr qint32 MAP_SIZE = 8;
constexpr qint32 PLAYER_COUNT = 2;
constexpr qint32 ACTOR_PLAYER = 0;
constexpr qint32 TARGET_PLAYER = 1;
constexpr qint32 SPAWN_RANGE_EXACT = 0;
constexpr bool SPAWN_IGNORE_MOVEMENT = true;
constexpr quint32 ACTION_SEED = 0x4A17C710u;
const QPoint ACTOR_ORIGIN(2, 2);
const QPoint FIRE_DESTINATION(2, 3);
const QPoint FIRE_TARGET(3, 3);
const QString INFANTRY_ID = QString(CoreAI::UNIT_INFANTRY);

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
    Player* actorPlayer{nullptr};
    Player* targetPlayer{nullptr};
    Unit* actor{nullptr};
    Unit* target{nullptr};
};

QByteArray serializeAction(const spGameAction & pAction, Checks & checks)
{
    QByteArray payload;
    if (pAction == nullptr)
    {
        checks.expect(false, QStringLiteral("cannot serialize null action"));
        return payload;
    }
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    pAction->serializeObject(stream);
    checks.expect(stream.status() == QDataStream::Ok,
                  QStringLiteral("action serialization failed"));
    return payload;
}

Fixture makeFixture(Checks & checks)
{
    Fixture fixture;
    fixture.map = MemoryManagement::create<GameMap>(MAP_SIZE, MAP_SIZE, PLAYER_COUNT);
    fixture.actorPlayer = fixture.map->getPlayer(ACTOR_PLAYER);
    fixture.targetPlayer = fixture.map->getPlayer(TARGET_PLAYER);
    checks.expect(fixture.actorPlayer != nullptr, QStringLiteral("missing actor player"));
    checks.expect(fixture.targetPlayer != nullptr, QStringLiteral("missing target player"));
    if (fixture.actorPlayer == nullptr || fixture.targetPlayer == nullptr)
    {
        return fixture;
    }
    fixture.actorPlayer->setTeam(ACTOR_PLAYER);
    fixture.targetPlayer->setTeam(TARGET_PLAYER);
    fixture.map->setCurrentPlayer(ACTOR_PLAYER);
    fixture.map->getGameRules()->setFogMode(GameEnums::Fog_Off);
    fixture.actor = fixture.map->spawnUnit(
        ACTOR_ORIGIN.x(), ACTOR_ORIGIN.y(), INFANTRY_ID, fixture.actorPlayer,
        SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    fixture.target = fixture.map->spawnUnit(
        FIRE_TARGET.x(), FIRE_TARGET.y(), INFANTRY_ID, fixture.targetPlayer,
        SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    checks.expect(fixture.actor != nullptr, QStringLiteral("actor spawn failed"));
    checks.expect(fixture.target != nullptr, QStringLiteral("target spawn failed"));
    checks.expect(fixture.actorPlayer->isEnemy(fixture.targetPlayer),
                  QStringLiteral("fixture players are not enemies"));
    return fixture;
}

Coordinator::PlannedAction firePlan(const Fixture & fixture)
{
    Coordinator::PlannedAction planned;
    planned.unitId = fixture.actor->getUniqueID();
    planned.kind = Coordinator::PlanBundleKind::MoveAndFire;
    planned.actionId = QString::fromLatin1(CoreAI::ACTION_FIRE);
    planned.path = {
        Coordinator::TilePoint{ACTOR_ORIGIN.x(), ACTOR_ORIGIN.y()},
        Coordinator::TilePoint{FIRE_DESTINATION.x(), FIRE_DESTINATION.y()},
    };
    planned.destination = Coordinator::TilePoint{
        FIRE_DESTINATION.x(), FIRE_DESTINATION.y()};
    planned.target = Coordinator::TilePoint{FIRE_TARGET.x(), FIRE_TARGET.y()};
    planned.targetUnitId = fixture.target->getUniqueID();
    return planned;
}

Coordinator::PlannedAction waitPlan(const Fixture & fixture)
{
    Coordinator::PlannedAction planned;
    planned.unitId = fixture.actor->getUniqueID();
    planned.kind = Coordinator::PlanBundleKind::Wait;
    planned.actionId = QString::fromLatin1(CoreAI::ACTION_WAIT);
    planned.path = {
        Coordinator::TilePoint{ACTOR_ORIGIN.x(), ACTOR_ORIGIN.y()},
    };
    planned.destination = Coordinator::TilePoint{
        ACTOR_ORIGIN.x(), ACTOR_ORIGIN.y()};
    return planned;
}

Coordinator::EngineActionBuildResult buildPinned(
    Fixture & fixture, const Coordinator::PlannedAction & planned)
{
    GameAction::pinActionSeeds(ACTION_SEED);
    const Coordinator::EngineActionBuildResult result =
        Coordinator::buildEngineAction(*fixture.map, *fixture.actorPlayer, planned);
    GameAction::unpinActionSeeds();
    return result;
}

void checkWaitConstruction(Fixture & fixture, Checks & checks)
{
    const Coordinator::EngineActionBuildResult result =
        buildPinned(fixture, waitPlan(fixture));
    checks.expect(static_cast<bool>(result), QStringLiteral("wait construction failed"));
    checks.expect(result.failure == Coordinator::EngineActionFailure::None,
                  QStringLiteral("wait construction reported a failure"));
    if (!result)
    {
        return;
    }
    checks.expect(result.action->getActionID() == QString::fromLatin1(CoreAI::ACTION_WAIT),
                  QStringLiteral("wait action id changed"));
    checks.expect(result.action->getTarget() == ACTOR_ORIGIN,
                  QStringLiteral("wait actor target changed"));
    checks.expect(result.action->getActionTarget() == ACTOR_ORIGIN,
                  QStringLiteral("wait movement target changed"));
    checks.expect(result.action->getMovePath() == QVector<QPoint>{ACTOR_ORIGIN},
                  QStringLiteral("wait path order changed"));
    checks.expect(result.action->canBePerformed(),
                  QStringLiteral("constructed wait is illegal"));
}

void checkFireConstruction(Fixture & fixture, Checks & checks)
{
    const Coordinator::PlannedAction planned = firePlan(fixture);
    const Coordinator::EngineActionBuildResult first = buildPinned(fixture, planned);
    const Coordinator::EngineActionBuildResult replay = buildPinned(fixture, planned);
    checks.expect(static_cast<bool>(first), QStringLiteral("fire construction failed"));
    checks.expect(static_cast<bool>(replay), QStringLiteral("fire replay failed"));
    if (!first || !replay)
    {
        return;
    }
    checks.expect(first.action->getActionID() == QString::fromLatin1(CoreAI::ACTION_FIRE),
                  QStringLiteral("fire action id changed"));
    checks.expect(first.action->getTarget() == ACTOR_ORIGIN,
                  QStringLiteral("fire actor target changed"));
    checks.expect(first.action->getActionTarget() == FIRE_DESTINATION,
                  QStringLiteral("fire movement target changed"));
    checks.expect(first.action->getMovePath() ==
                      QVector<QPoint>{FIRE_DESTINATION, ACTOR_ORIGIN},
                  QStringLiteral("fire path order changed"));
    checks.expect(first.action->getInputStep() == 1,
                  QStringLiteral("fire target input step changed"));
    checks.expect(first.action->canBePerformed(),
                  QStringLiteral("constructed fire is illegal"));
    checks.expect(first.action->getSeed() == replay.action->getSeed(),
                  QStringLiteral("replayed action seed changed"));
    checks.expect(serializeAction(first.action, checks) ==
                      serializeAction(replay.action, checks),
                  QStringLiteral("replayed action payload changed"));
}

void checkRejections(Fixture & fixture, Checks & checks)
{
    Coordinator::PlannedAction malformed = waitPlan(fixture);
    malformed.path.clear();
    const auto malformedResult =
        Coordinator::buildEngineAction(*fixture.map, *fixture.actorPlayer, malformed);
    checks.expect(!malformedResult &&
                      malformedResult.failure == Coordinator::EngineActionFailure::InvalidShape,
                  QStringLiteral("malformed action was accepted"));

    Coordinator::PlannedAction wrongOrigin = waitPlan(fixture);
    wrongOrigin.path.front() = Coordinator::TilePoint{
        ACTOR_ORIGIN.x() + 1, ACTOR_ORIGIN.y()};
    wrongOrigin.destination = wrongOrigin.path.front();
    const auto originResult =
        Coordinator::buildEngineAction(*fixture.map, *fixture.actorPlayer, wrongOrigin);
    checks.expect(!originResult &&
                      originResult.failure == Coordinator::EngineActionFailure::OriginMismatch,
                  QStringLiteral("origin mismatch was accepted"));

    fixture.actor->setHasMoved(true);
    const auto movedResult = Coordinator::buildEngineAction(
        *fixture.map, *fixture.actorPlayer, waitPlan(fixture));
    fixture.actor->setHasMoved(false);
    checks.expect(!movedResult &&
                      movedResult.failure == Coordinator::EngineActionFailure::ActorUnavailable,
                  QStringLiteral("moved actor was accepted"));

    Coordinator::PlannedAction missingTarget = firePlan(fixture);
    missingTarget.targetUnitId = Coordinator::NO_UNIT;
    const auto targetResult = Coordinator::buildEngineAction(
        *fixture.map, *fixture.actorPlayer, missingTarget);
    checks.expect(!targetResult &&
                      targetResult.failure == Coordinator::EngineActionFailure::TargetUnavailable,
                  QStringLiteral("changed fire target was accepted"));
}
}

QVariantMap ActionConstructionProbe::run(QObject*, const QVariantMap &)
{
    Checks checks;
    GameAction::unpinActionSeeds();
    Fixture fixture = makeFixture(checks);
    if (fixture.actor != nullptr && fixture.target != nullptr)
    {
        checkWaitConstruction(fixture, checks);
        checkFireConstruction(fixture, checks);
        checkRejections(fixture, checks);
    }
    GameAction::unpinActionSeeds();
    return {
        {QStringLiteral("ok"), checks.failures.isEmpty()},
        {QStringLiteral("failures"), checks.failures},
    };
}

namespace
{
[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("coordinatedActionConstruction"), ActionConstructionProbe::run);
}
