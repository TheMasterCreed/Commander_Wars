#include "tests/arena/actionseedprobe.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QJSValue>
#include <QRandomGenerator>
#include <QStringList>
#include <QVector>

#include "coreengine/globalutils.h"
#include "coreengine/interpreter.h"
#include "coreengine/memorymanagement.h"
#include "game/gameaction.h"
#include "tests/arena/arenatestsupport.h"

namespace
{
constexpr qint32 PINNED_ACTION_COUNT = 8;
constexpr quint32 MASTER_SEED = 0x35A17EEDu;
constexpr quint32 ALTERNATE_MASTER_SEED = 0x2468ACE0u;
constexpr quint32 TRANSITION_SEED = 0x5EED1234u;
const QString ACTION_ID = QStringLiteral("ACTION_ARENA_ACTION_SEED");

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

QVector<quint32> collectPinnedSeeds(quint32 masterSeed)
{
    QVector<quint32> seeds;
    seeds.reserve(PINNED_ACTION_COUNT);
    GameAction::pinActionSeeds(masterSeed);
    for (qint32 i = 0; i < PINNED_ACTION_COUNT; ++i)
    {
        spGameAction pAction;
        if (i % 2 == 0)
        {
            pAction = MemoryManagement::create<GameAction>(nullptr);
        }
        else
        {
            pAction = MemoryManagement::create<GameAction>(ACTION_ID, nullptr);
        }
        seeds.append(pAction->getSeed());
    }
    GameAction::unpinActionSeeds();
    return seeds;
}

QByteArray serializeAction(const GameAction & action, Checks & checks)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    action.serializeObject(stream);
    checks.expect(stream.status() == QDataStream::Ok,
                  QStringLiteral("action serialization failed"));
    return data;
}

spGameAction restoreAction(const QByteArray & data, Checks & checks)
{
    spGameAction pAction = MemoryManagement::create<GameAction>(nullptr);
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_5);
    pAction->deserializeObject(stream);
    checks.expect(stream.status() == QDataStream::Ok,
                  QStringLiteral("action restoration failed"));
    return pAction;
}

bool installTransition(Checks & checks)
{
    const QJSValue result = Interpreter::getInstance()->doString(
        "var ARENA_ACTION_SEED_TRACE = [];"
        "var ACTION_ARENA_ACTION_SEED = {"
        "  isFinalStep: function() {"
        "    var value = globals.randInt(0, 2147483646);"
        "    ARENA_ACTION_SEED_TRACE.push(value);"
        "    return (value & 1) === 0;"
        "  }"
        "};");
    checks.expect(!result.isError(), QStringLiteral("transition setup failed"));
    return !result.isError();
}

QVector<qint32> transitionTrace()
{
    QVector<qint32> values;
    const QJSValue trace =
        Interpreter::getInstance()->doString(QStringLiteral("ARENA_ACTION_SEED_TRACE"));
    const qint32 length = trace.property(QStringLiteral("length")).toInt();
    values.reserve(length);
    for (qint32 i = 0; i < length; ++i)
    {
        values.append(trace.property(i).toInt());
    }
    return values;
}

void testPinnedConstruction(Checks & checks)
{
    const QVector<quint32> first = collectPinnedSeeds(MASTER_SEED);
    const QVector<quint32> replay = collectPinnedSeeds(MASTER_SEED);
    const QVector<quint32> alternate = collectPinnedSeeds(ALTERNATE_MASTER_SEED);
    checks.expect(first.size() == PINNED_ACTION_COUNT,
                  QStringLiteral("pinned sequence length changed"));
    checks.expect(first == replay,
                  QStringLiteral("pinned sequence did not replay"));
    checks.expect(first != alternate,
                  QStringLiteral("alternate master seed matched"));
    checks.expect(!GameAction::getActionSeedsPinned(),
                  QStringLiteral("pinned state was not restored"));
}

void testSerializedTransition(Checks & checks)
{
    if (!installTransition(checks))
    {
        return;
    }
    spGameAction pOriginal =
        MemoryManagement::create<GameAction>(ACTION_ID, nullptr, TRANSITION_SEED);
    spGameAction pRestored = restoreAction(serializeAction(*pOriginal, checks), checks);
    checks.expect(pRestored->getSeed() == TRANSITION_SEED,
                  QStringLiteral("serialized seed changed"));
    checks.expect(pRestored->getActionID() == ACTION_ID,
                  QStringLiteral("serialized action identity changed"));

    GlobalUtils::setUseSeed(false);
    const bool originalResult = pOriginal->isFinalStep();
    checks.expect(!GlobalUtils::getUseSeed(),
                  QStringLiteral("original transition kept seed ownership"));
    (void)QRandomGenerator::global()->generate();
    const bool restoredResult = pRestored->isFinalStep();
    checks.expect(!GlobalUtils::getUseSeed(),
                  QStringLiteral("restored transition kept seed ownership"));

    const QVector<qint32> trace = transitionTrace();
    checks.expect(originalResult == restoredResult,
                  QStringLiteral("restored transition result changed"));
    checks.expect(trace.size() == 2,
                  QStringLiteral("transition trace length changed"));
    if (trace.size() == 2)
    {
        checks.expect(trace[0] == trace[1],
                      QStringLiteral("restored transition used another RNG"));
    }
    Interpreter::getInstance()->doString(
        "ARENA_ACTION_SEED_TRACE = undefined;"
        "ACTION_ARENA_ACTION_SEED = undefined;");
}
}

QVariantMap ActionSeedProbe::run(QObject*, const QVariantMap &)
{
    Checks checks;
    GameAction::unpinActionSeeds();
    testPinnedConstruction(checks);
    testSerializedTransition(checks);
    GameAction::unpinActionSeeds();
    GlobalUtils::setUseSeed(false);
    return {
        {QStringLiteral("ok"), checks.failures.isEmpty()},
        {QStringLiteral("failures"), checks.failures},
    };
}

namespace
{
[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("actionSeedDeterminism"), ActionSeedProbe::run);
}
