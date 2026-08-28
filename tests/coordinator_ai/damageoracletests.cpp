#include <algorithm>

#include <QPoint>
#include <QRectF>
#include <QStringList>
#include <QVariantMap>

#include "ai/coordinator/damageoracle.h"
#include "ai/coreai.h"
#include "coreengine/memorymanagement.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "resource_management/weaponmanager.h"
#include "tests/arena/arenatestsupport.h"

namespace
{
constexpr bool MAP_ONLY_LOAD = true;
constexpr bool MAP_FAST_LOAD = false;
constexpr bool MAP_IS_SAVEGAME = false;
constexpr qint32 ATTACKER_PLAYER = 0;
constexpr qint32 DEFENDER_PLAYER = 1;
constexpr qint32 SPAWN_RANGE_EXACT = 0;
constexpr bool SPAWN_IGNORE_MOVEMENT = true;
constexpr float NO_TAKEN_DAMAGE = 0.0f;
constexpr bool IGNORE_OUT_OF_VISION_RANGE = false;
constexpr bool ACCURATE_DAMAGE = false;
constexpr double NO_WEAPON_DAMAGE = -1.0;
const QString MAP_PATH = QStringLiteral("maps/2_player/Plug Mountain.map");
const QString LIGHT_TANK = QStringLiteral("LIGHT_TANK");
const QString ARTILLERY = QStringLiteral("ARTILLERY");

void expect(bool condition, const QString & message, QStringList & failures)
{
    if (!condition)
    {
        failures.append(message);
    }
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
                free = free && map.getTerrain(x + offset, y)->getUnit() == nullptr;
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

void checkRounding(QStringList & failures)
{
    expect(Coordinator::ceilHpSteps(7.0) == 7, QStringLiteral("whole hp rounded up"), failures);
    expect(Coordinator::ceilHpSteps(7.01) == 8, QStringLiteral("fractional hp not rounded up"), failures);
    expect(Coordinator::damageStepsFrom(7.5, 6.0) == 1, QStringLiteral("partial loss rounding"), failures);
    expect(Coordinator::damageStepsFrom(7.5, 75.0) == 8, QStringLiteral("lethal loss rounding"), failures);
    expect(Coordinator::damageStepsFrom(7.5, -1.0) == 0, QStringLiteral("impossible shot caused loss"), failures);
}

void checkWeaponSelection(Coordinator::DamageOracle & oracle, Unit & tank, Unit & artillery, Unit & defender,
                          QStringList & failures)
{
    WeaponManager* pWeapons = WeaponManager::getInstance();
    const double gun = pWeapons->getBaseDamage(tank.getWeapon1ID(), &defender);
    const double machineGun = pWeapons->getBaseDamage(tank.getWeapon2ID(), &defender);
    expect(oracle.baseDamageAgainst(tank, defender, 1, 1, 1) == std::max(gun, machineGun),
           QStringLiteral("best direct weapon not selected"), failures);
    tank.setAmmo1(0);
    expect(oracle.baseDamageAgainst(tank, defender, 1, 1, 1) == machineGun,
           QStringLiteral("empty primary weapon selected"), failures);
    const double indirect = pWeapons->getBaseDamage(artillery.getWeapon1ID(), &defender);
    expect(oracle.baseDamageAgainst(artillery, defender, 1, 2, 3) == NO_WEAPON_DAMAGE,
           QStringLiteral("indirect weapon fired adjacent"), failures);
    expect(oracle.baseDamageAgainst(artillery, defender, 2, 2, 3) == indirect,
           QStringLiteral("indirect weapon rejected in range"), failures);
    artillery.setAmmo1(0);
    expect(oracle.baseDamageAgainst(artillery, defender, 2, 2, 3) == NO_WEAPON_DAMAGE,
           QStringLiteral("empty indirect weapon selected"), failures);
}

QVariantMap damageOracle(QObject*, const QVariantMap &)
{
    QStringList failures;
    checkRounding(failures);
    spGameMap pMap = MemoryManagement::create<GameMap>(
        MAP_PATH, MAP_ONLY_LOAD, MAP_FAST_LOAD, MAP_IS_SAVEGAME);
    Player* pAttackerPlayer = pMap->getPlayer(ATTACKER_PLAYER);
    Player* pDefenderPlayer = pMap->getPlayer(DEFENDER_PLAYER);
    qint32 x = -1;
    qint32 y = -1;
    expect(pMap->getGameRules() != nullptr && pAttackerPlayer != nullptr &&
               pDefenderPlayer != nullptr && pAttackerPlayer->isEnemy(pDefenderPlayer),
           QStringLiteral("invalid fixture"), failures);
    expect(findFreeRow(*pMap, 3, x, y), QStringLiteral("no free spawn row"), failures);
    if (!failures.isEmpty())
    {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("failures"), failures}};
    }
    Unit* pAttacker = pMap->spawnUnit(
        x, y, LIGHT_TANK, pAttackerPlayer, SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    Unit* pDefender = pMap->spawnUnit(
        x + 1, y, LIGHT_TANK, pDefenderPlayer, SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    Unit* pArtillery = pMap->spawnUnit(
        x + 2, y, ARTILLERY, pAttackerPlayer, SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    expect(pAttacker != nullptr && pDefender != nullptr && pArtillery != nullptr,
           QStringLiteral("unit spawn failed"), failures);
    if (!failures.isEmpty())
    {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("failures"), failures}};
    }
    Coordinator::DamageOracle oracle(*pMap);
    const QPoint attackerTile(x, y);
    const QPoint defenderTile(x + 1, y);
    const QRectF report = CoreAI::calcVirtuelUnitDamage(
        pMap.get(), pAttacker, NO_TAKEN_DAMAGE, attackerTile, GameEnums::LuckDamageMode_Average,
        pDefender, NO_TAKEN_DAMAGE, defenderTile, GameEnums::LuckDamageMode_Average,
        IGNORE_OUT_OF_VISION_RANGE, ACCURATE_DAMAGE);
    const Coordinator::DamageEstimate estimate = oracle.estimate(
        0, *pAttacker, {x, y}, 1, *pDefender, {x + 1, y});
    expect(estimate.possible && report.x() >= 0, QStringLiteral("legal attack rejected"), failures);
    expect(estimate.damageSteps == Coordinator::damageStepsFrom(pDefender->getHp(), report.x()),
           QStringLiteral("average attack loss mismatch"), failures);
    expect(estimate.counterSteps == Coordinator::damageStepsFrom(pAttacker->getHp(), report.width()),
           QStringLiteral("average counter loss mismatch"), failures);
    const Coordinator::DamageEstimate repeated = oracle.estimate(
        0, *pAttacker, {x, y}, 1, *pDefender, {x + 1, y});
    expect(repeated.damageSteps == estimate.damageSteps && repeated.counterSteps == estimate.counterSteps &&
               oracle.callCount() == 2 && oracle.hitCount() == 1,
           QStringLiteral("memo changed estimate"), failures);
    oracle.clear();
    expect(oracle.callCount() == 0 && oracle.hitCount() == 0,
           QStringLiteral("clear kept memo counters"), failures);
    checkWeaponSelection(oracle, *pAttacker, *pArtillery, *pDefender, failures);
    return {{QStringLiteral("ok"), failures.isEmpty()}, {QStringLiteral("failures"), failures}};
}

[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("damageOracle"), damageOracle);
}
