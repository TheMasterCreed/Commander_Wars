#include "tests/arena/capturebonusprobe.h"

#include <QByteArray>
#include <QDataStream>
#include <QJSValue>
#include <QPoint>
#include <QStringList>
#include <QVector>

#include "ai/coreai.h"
#include "coreengine/interpreter.h"
#include "coreengine/memorymanagement.h"
#include "coreengine/scriptvariable.h"
#include "coreengine/scriptvariables.h"
#include "game/co.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/unit.h"
#include "tests/arena/arenatestsupport.h"

namespace
{
constexpr qint32 MAP_WIDTH = 8;
constexpr qint32 MAP_HEIGHT = 8;
constexpr qint32 MAP_PLAYER_COUNT = 2;
constexpr qint32 OWNER_PLAYER = 0;
constexpr qint32 REPEATED_QUERY_COUNT = 5;
constexpr qint32 SPAWN_RANGE_EXACT = 0;
constexpr bool SPAWN_IGNORE_MOVEMENT = true;
constexpr bool FOR_HASH = true;
constexpr bool FULL_SAVE = false;

const QString MARY_CO_ID = QStringLiteral("CO_MARY");
const QString OTHER_CO_ID = QStringLiteral("CO_ANDY");
const QString TEST_PERK_ID = QStringLiteral("CO_PERK_LUCK_5");
const QString STORED_X_VARIABLE = QStringLiteral("POSXBUILDINGS");
const QString STORED_Y_VARIABLE = QStringLiteral("POSYBUILDINGS");
const QPoint UNIT_TILE{1, 1};
const QPoint BONUS_TILE{2, 2};
const QPoint TAIL_TILE{3, 3};
const QPoint PLAIN_TILE{4, 2};
const QPoint UNPAID_TILE{6, 2};

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

struct MaryFixture
{
    spGameMap pMap;
    CO* pMary{nullptr};
    Unit* pUnit{nullptr};
};

bool buildFixture(qint32 marySlot, MaryFixture & fixture)
{
    fixture.pMap = MemoryManagement::create<GameMap>(MAP_WIDTH, MAP_HEIGHT, MAP_PLAYER_COUNT);
    GameRules* pRules = fixture.pMap->getGameRules();
    Player* pPlayer = fixture.pMap->getPlayer(OWNER_PLAYER);
    if (pRules == nullptr || pPlayer == nullptr)
    {
        return false;
    }
    pRules->setParallelCos(true);
    pRules->setEnableDayToDayCoAbilities(true);
    pRules->setCoGlobalD2D(false);
    pPlayer->setCO(marySlot == 0 ? MARY_CO_ID : OTHER_CO_ID, 0);
    pPlayer->setCO(marySlot == 1 ? MARY_CO_ID : OTHER_CO_ID, 1);
    fixture.pMary = pPlayer->getCO(marySlot);
    fixture.pUnit = fixture.pMap->spawnUnit(
        UNIT_TILE.x(), UNIT_TILE.y(), CoreAI::UNIT_INFANTRY, pPlayer,
        SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    return fixture.pMary != nullptr && fixture.pUnit != nullptr;
}

QByteArray serializeVariables(ScriptVariables & variables, bool forHash)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    variables.serializeObject(stream, forHash);
    return data;
}

void storeCaptureBonus(CO & co, QPoint position)
{
    ScriptVariables* pVariables = co.getVariables();
    ScriptVariable* pStoredX = pVariables->createVariable(STORED_X_VARIABLE);
    ScriptVariable* pStoredY = pVariables->createVariable(STORED_Y_VARIABLE);
    QVector<qint32> pointsX = pStoredX->readDataListInt32();
    QVector<qint32> pointsY = pStoredY->readDataListInt32();
    pointsX.append(position.x());
    pointsY.append(position.y());
    pStoredX->writeDataListInt32(pointsX);
    pStoredY->writeDataListInt32(pointsY);
}

void testMarySlot(qint32 marySlot, Checks & checks)
{
    MaryFixture fixture;
    if (!buildFixture(marySlot, fixture))
    {
        checks.failures.append("slot " + QString::number(marySlot) + " setup failed");
        return;
    }
    ScriptVariables* pVariables = fixture.pMary->getVariables();
    const QByteArray emptyHash = serializeVariables(*pVariables, FOR_HASH);
    for (qint32 i = 0; i < REPEATED_QUERY_COUNT; ++i)
    {
        fixture.pUnit->getCaptureRate(BONUS_TILE);
    }
    checks.expect(pVariables->getVariable(STORED_X_VARIABLE) == nullptr,
                  "slot " + QString::number(marySlot) + " query created x storage");
    checks.expect(pVariables->getVariable(STORED_Y_VARIABLE) == nullptr,
                  "slot " + QString::number(marySlot) + " query created y storage");
    checks.expect(serializeVariables(*pVariables, FOR_HASH) == emptyHash,
                  "slot " + QString::number(marySlot) + " empty query mutated state");

    fixture.pMary->setPowerMode(GameEnums::PowerMode_Power);
    const qint32 plainRate = fixture.pUnit->getCaptureRate(PLAIN_TILE);
    storeCaptureBonus(*fixture.pMary, BONUS_TILE);
    storeCaptureBonus(*fixture.pMary, TAIL_TILE);
    const QByteArray storedHash = serializeVariables(*pVariables, FOR_HASH);
    const QByteArray storedSave = serializeVariables(*pVariables, FULL_SAVE);
    const qint32 bonusRate = fixture.pUnit->getCaptureRate(BONUS_TILE);
    checks.expect(bonusRate > plainRate,
                  "slot " + QString::number(marySlot) + " stored bonus was not paid");
    for (qint32 i = 0; i < REPEATED_QUERY_COUNT; ++i)
    {
        checks.expect(fixture.pUnit->getCaptureRate(BONUS_TILE) == bonusRate,
                      "slot " + QString::number(marySlot) + " query was not repeatable");
    }
    checks.expect(serializeVariables(*pVariables, FOR_HASH) == storedHash,
                  "slot " + QString::number(marySlot) + " query changed hash state");
    checks.expect(serializeVariables(*pVariables, FULL_SAVE) == storedSave,
                  "slot " + QString::number(marySlot) + " query consumed storage");

    const qint32 beforeCapture = fixture.pUnit->getCapturePoints();
    fixture.pUnit->increaseCapturePoints(BONUS_TILE);
    checks.expect(fixture.pUnit->getCapturePoints() - beforeCapture == bonusRate,
                  "slot " + QString::number(marySlot) + " capture paid the wrong rate");
    ScriptVariable* pStoredX = pVariables->getVariable(STORED_X_VARIABLE);
    ScriptVariable* pStoredY = pVariables->getVariable(STORED_Y_VARIABLE);
    checks.expect(pStoredX != nullptr && pStoredY != nullptr,
                  "slot " + QString::number(marySlot) + " lost paired storage");
    if (pStoredX != nullptr && pStoredY != nullptr)
    {
        checks.expect(pStoredX->readDataListInt32() == QVector<qint32>{TAIL_TILE.x()},
                      "slot " + QString::number(marySlot) + " removed the wrong x");
        checks.expect(pStoredY->readDataListInt32() == QVector<qint32>{TAIL_TILE.y()},
                      "slot " + QString::number(marySlot) + " removed the wrong y");
    }
    checks.expect(fixture.pUnit->getCaptureRate(BONUS_TILE) == plainRate,
                  "slot " + QString::number(marySlot) + " entry was not consumed");
    const qint32 afterFirstCapture = fixture.pUnit->getCapturePoints();
    fixture.pUnit->increaseCapturePoints(BONUS_TILE);
    checks.expect(fixture.pUnit->getCapturePoints() - afterFirstCapture == plainRate,
                  "slot " + QString::number(marySlot) + " entry paid twice");
}

void testZeroPayConsumption(Checks & checks)
{
    MaryFixture fixture;
    if (!buildFixture(0, fixture))
    {
        checks.failures.append("zero-pay setup failed");
        return;
    }
    fixture.pMary->setPowerMode(GameEnums::PowerMode_Off);
    storeCaptureBonus(*fixture.pMary, UNPAID_TILE);
    const qint32 plainRate = fixture.pUnit->getCaptureRate(PLAIN_TILE);
    checks.expect(fixture.pUnit->getCaptureRate(UNPAID_TILE) == plainRate,
                  "zero-pay entry produced a bonus");
    fixture.pUnit->increaseCapturePoints(UNPAID_TILE);
    ScriptVariables* pVariables = fixture.pMary->getVariables();
    checks.expect(pVariables->getVariable(STORED_X_VARIABLE)->readDataListInt32().isEmpty(),
                  "zero-pay capture kept x");
    checks.expect(pVariables->getVariable(STORED_Y_VARIABLE)->readDataListInt32().isEmpty(),
                  "zero-pay capture kept y");
    fixture.pMary->setPowerMode(GameEnums::PowerMode_Power);
    const qint32 poweredPlainRate = fixture.pUnit->getCaptureRate(PLAIN_TILE);
    checks.expect(fixture.pUnit->getCaptureRate(UNPAID_TILE) == poweredPlainRate,
                  "zero-pay entry became payable later");
}

void testTagAndPerkDispatch(Checks & checks)
{
    MaryFixture fixture;
    if (!buildFixture(0, fixture))
    {
        checks.failures.append("dispatch setup failed");
        return;
    }
    fixture.pMary->addPerk(TEST_PERK_ID);
    Interpreter* pInterpreter = Interpreter::getInstance();
    const QJSValue setup = pInterpreter->doString(
        "var ARENA_CAPTURE_DISPATCH = { tag: 0, perk: 0 };"
        "TAGPOWER.consumeCaptureBonus = function() { ++ARENA_CAPTURE_DISPATCH.tag; };"
        "CO_PERK_LUCK_5.consumeCaptureBonus = function() { ++ARENA_CAPTURE_DISPATCH.perk; };");
    checks.expect(!setup.isError(), "dispatch hooks failed to install");
    fixture.pUnit->increaseCapturePoints(PLAIN_TILE);
    const QJSValue tagCount = pInterpreter->doString("ARENA_CAPTURE_DISPATCH.tag");
    const QJSValue perkCount = pInterpreter->doString("ARENA_CAPTURE_DISPATCH.perk");
    checks.expect(tagCount.isNumber() && tagCount.toInt() == 2,
                  "tag dispatch did not cover both co slots");
    checks.expect(perkCount.isNumber() && perkCount.toInt() == 1,
                  "perk dispatch count was wrong");
    pInterpreter->doString(
        "TAGPOWER.consumeCaptureBonus = null;"
        "CO_PERK_LUCK_5.consumeCaptureBonus = null;"
        "ARENA_CAPTURE_DISPATCH = undefined;");
}
}

QVariantMap CaptureBonusProbe::run(QObject*, const QVariantMap &)
{
    Checks checks;
    testMarySlot(0, checks);
    testMarySlot(1, checks);
    testZeroPayConsumption(checks);
    testTagAndPerkDispatch(checks);
    return {
        {"ok", checks.failures.isEmpty()},
        {"failures", checks.failures},
    };
}

namespace
{
[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("captureBonusPurity"), CaptureBonusProbe::run);
}
