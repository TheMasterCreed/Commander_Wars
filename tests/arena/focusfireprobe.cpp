#include "tests/arena/focusfireprobe.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <vector>

#include <QStringList>

#include "ai/coreai.h"
#include "ai/coordinator/attackopportunitybuilder.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/propertyeconomicsbuilder.h"
#include "ai/coordinator/propertystockbuilder.h"
#include "coreengine/memorymanagement.h"
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
constexpr bool SKIP_HP_VISUALS = false;
constexpr qint32 ENEMY_COUNT = 3;
constexpr qint32 ENEMY_DISTANCE = 2;
const QPoint PROFILE_TILE(0, 0);
const QString UNIT_ID = QString(CoreAI::UNIT_INFANTRY);
constexpr std::array<qreal, ENEMY_COUNT> ENEMY_HP{4.0, 7.0, 10.0};

struct Profile
{
    qint32 movementPoints{0};
    qint32 minRange{0};
    qint32 maxRange{0};
};

struct Layout
{
    QPoint actor;
    std::vector<QPoint> enemies;
};

bool tileFree(GameMap & map, qint32 x, qint32 y)
{
    Terrain* pTerrain = map.getTerrain(x, y);
    return pTerrain != nullptr && pTerrain->getUnit() == nullptr;
}

bool tilePassable(const Coordinator::MobilityCostGrid & grid, qint32 x, qint32 y)
{
    for (const Coordinator::EntryDirection direction : Coordinator::ENTRY_DIRECTIONS)
    {
        if (grid.cost(x, y, direction) >= 0)
        {
            return true;
        }
    }
    return false;
}

Coordinator::DistanceField reachFrom(const Coordinator::MobilityCostGrid & grid, const QPoint & tile)
{
    return Coordinator::DistanceField::build(
        grid, {Coordinator::TilePoint{tile.x(), tile.y()}}, Coordinator::FieldExpansion::FromSources);
}

qint32 coveringCount(const std::vector<Coordinator::DistanceField> & reaches, const Profile & profile,
                     qint32 x, qint32 y)
{
    qint32 count = 0;
    for (const Coordinator::DistanceField & reach : reaches)
    {
        if (Coordinator::movedShotDistance(reach, profile.movementPoints, profile.minRange, profile.maxRange,
                                           Coordinator::TilePoint{x, y}) != Coordinator::NO_SHOT_DISTANCE)
        {
            ++count;
        }
    }
    return count;
}

bool findLayout(GameMap & map, const Coordinator::MobilityCostGrid & grid, const Profile & profile, Layout & layout)
{
    for (qint32 actorY = 0; actorY < map.getMapHeight(); ++actorY)
    {
        for (qint32 actorX = 0; actorX < map.getMapWidth(); ++actorX)
        {
            if (!tileFree(map, actorX, actorY) || !tilePassable(grid, actorX, actorY))
            {
                continue;
            }
            std::vector<QPoint> enemies;
            for (qint32 dy = -ENEMY_DISTANCE; dy <= ENEMY_DISTANCE; ++dy)
            {
                for (qint32 dx = -ENEMY_DISTANCE; dx <= ENEMY_DISTANCE; ++dx)
                {
                    const qint32 x = actorX + dx;
                    const qint32 y = actorY + dy;
                    if (enemies.size() >= ENEMY_COUNT || qAbs(dx) + qAbs(dy) != ENEMY_DISTANCE ||
                        !map.onMap(x, y) || !tileFree(map, x, y) || !tilePassable(grid, x, y))
                    {
                        continue;
                    }
                    enemies.emplace_back(x, y);
                }
            }
            if (enemies.size() != ENEMY_COUNT)
            {
                continue;
            }
            std::vector<Coordinator::DistanceField> enemyReach;
            for (const QPoint & enemy : enemies)
            {
                enemyReach.push_back(reachFrom(grid, enemy));
            }
            if (coveringCount(enemyReach, profile, actorX, actorY) != ENEMY_COUNT)
            {
                continue;
            }
            Coordinator::MobilityCostGrid blocked = grid;
            for (const QPoint & enemy : enemies)
            {
                blocked.setTileCost(enemy.x(), enemy.y(), Coordinator::IMPASSABLE_COST);
            }
            const Coordinator::DistanceField actorReach = reachFrom(blocked, QPoint(actorX, actorY));
            bool escape = false;
            for (qint32 y = 0; y < map.getMapHeight() && !escape; ++y)
            {
                for (qint32 x = 0; x < map.getMapWidth(); ++x)
                {
                    const QPoint tile(x, y);
                    if (tile == QPoint(actorX, actorY) ||
                        std::find(enemies.begin(), enemies.end(), tile) != enemies.end() ||
                        !tileFree(map, x, y) || !actorReach.reachable(x, y, profile.movementPoints))
                    {
                        continue;
                    }
                    if (coveringCount(enemyReach, profile, x, y) < ENEMY_COUNT)
                    {
                        escape = true;
                        break;
                    }
                }
            }
            if (escape)
            {
                layout = Layout{QPoint(actorX, actorY), std::move(enemies)};
                return true;
            }
        }
    }
    return false;
}

const Coordinator::CandidateBundle* bundleOfKind(const std::vector<Coordinator::CandidateBundle> & candidates,
                                                 Coordinator::PlanBundleKind kind)
{
    for (const Coordinator::CandidateBundle & candidate : candidates)
    {
        if (Coordinator::planBundleKindOf(candidate.bundle) == kind)
        {
            return &candidate;
        }
    }
    return nullptr;
}

QString checkExposure(const std::vector<Coordinator::CandidateBundle> & candidates)
{
    const Coordinator::CandidateBundle* pWait = bundleOfKind(candidates, Coordinator::PlanBundleKind::Wait);
    if (pWait == nullptr)
    {
        return QStringLiteral("no-wait-bundle");
    }
    const std::vector<Coordinator::MilliFunds> & origin = pWait->enemyShotsOnActorAtOrigin;
    if (origin.size() != ENEMY_COUNT)
    {
        return QStringLiteral("origin-shot-count:") + QString::number(origin.size());
    }
    const Coordinator::MilliFunds strongest = *std::max_element(origin.begin(), origin.end());
    const Coordinator::MilliFunds sum = std::accumulate(origin.begin(), origin.end(), Coordinator::MilliFunds{0});
    if (strongest <= 0 || Coordinator::bestShot(origin) != strongest)
    {
        return QStringLiteral("best-shot-is-not-maximum");
    }
    if (strongest == origin.front() || strongest == sum)
    {
        return QStringLiteral("shot-profile-not-distinct");
    }
    if (pWait->valuation.continuation.exposure != 0)
    {
        return QStringLiteral("wait-exposure-changed");
    }
    for (const Coordinator::CandidateBundle & candidate : candidates)
    {
        if (Coordinator::planBundleKindOf(candidate.bundle) != Coordinator::PlanBundleKind::Move ||
            candidate.enemyShotsOnActorAtDestination.size() >= origin.size())
        {
            continue;
        }
        const Coordinator::MilliFunds expected =
            Coordinator::bestShot(candidate.enemyShotsOnActorAtOrigin) -
            Coordinator::bestShot(candidate.enemyShotsOnActorAtDestination);
        if (candidate.valuation.continuation.exposure != expected)
        {
            return QStringLiteral("move-exposure:") + QString::number(candidate.valuation.continuation.exposure) +
                   QStringLiteral(":expected:") + QString::number(expected);
        }
        return QString();
    }
    return QStringLiteral("no-less-covered-move");
}

QString focusFireExposure(const QString & mapPath)
{
    spGameMap pMap = MemoryManagement::create<GameMap>(mapPath, MAP_ONLY_LOAD, MAP_FAST_LOAD, MAP_IS_SAVEGAME);
    GameRules* pRules = pMap->getGameRules();
    Player* pObserver = pMap->getPlayer(OBSERVER_PLAYER);
    Player* pEnemy = pMap->getPlayer(ENEMY_PLAYER);
    if (pRules == nullptr || pObserver == nullptr || pEnemy == nullptr || !pObserver->isEnemy(pEnemy))
    {
        return QStringLiteral("invalid-map");
    }
    pRules->setFogMode(GameEnums::Fog_Off);
    spUnit pDummy = MemoryManagement::create<Unit>(UNIT_ID, pObserver, false, pMap.get());
    if (!pDummy->hasWeapons() || !pDummy->canMoveAndFire(PROFILE_TILE))
    {
        return QStringLiteral("invalid-probe-unit");
    }
    const Profile profile{
        pDummy->getMovementpoints(PROFILE_TILE),
        pDummy->getBaseMinRange(),
        pDummy->getBaseMaxRange(),
    };
    Coordinator::MobilityFieldCache cache;
    Layout layout;
    if (!findLayout(*pMap, cache.grid(*pMap, *pObserver, UNIT_ID), profile, layout))
    {
        return QStringLiteral("no-focus-fire-layout");
    }
    Unit* pActor = pMap->spawnUnit(layout.actor.x(), layout.actor.y(), UNIT_ID, pObserver,
                                   SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
    if (pActor == nullptr)
    {
        return QStringLiteral("actor-spawn-failed");
    }
    for (std::size_t i = 0; i < layout.enemies.size(); ++i)
    {
        const QPoint tile = layout.enemies[i];
        Unit* pSpawned = pMap->spawnUnit(tile.x(), tile.y(), UNIT_ID, pEnemy,
                                         SPAWN_RANGE_EXACT, SPAWN_IGNORE_MOVEMENT);
        if (pSpawned == nullptr)
        {
            return QStringLiteral("enemy-spawn-failed");
        }
        pSpawned->setHp(ENEMY_HP[i], SKIP_HP_VISUALS);
    }
    cache.clear();
    const Coordinator::BattlefieldKnowledge knowledge =
        Coordinator::BattlefieldKnowledge::capture(*pMap, *pObserver);
    cache.grid(*pMap, *pObserver, UNIT_ID);
    const Coordinator::AttackOpportunityField field =
        Coordinator::buildAttackOpportunityField(*pMap, knowledge, cache);
    if (field.attackerSlotsAt(layout.actor.x(), layout.actor.y()).size() != ENEMY_COUNT)
    {
        return QStringLiteral("actor-not-covered-by-three");
    }
    const Coordinator::KnownUnit* pKnownActor = knowledge.unitAt(layout.actor.x(), layout.actor.y());
    if (pKnownActor == nullptr)
    {
        return QStringLiteral("actor-missing");
    }
    const std::vector<Coordinator::PropertyFacts> properties =
        Coordinator::buildPropertyEconomics(*pMap, knowledge);
    const Coordinator::ValuationContext valuation = Coordinator::makeValuationContext(*pMap);
    Coordinator::PropertyStockField propertyStock =
        Coordinator::buildPropertyStockField(*pMap, knowledge, properties, cache, valuation.horizonTurns);
    Coordinator::DamageOracle oracle(*pMap);
    const Coordinator::BundleBuildContext context{
        *pMap,
        *pObserver,
        knowledge,
        field,
        properties,
        cache,
        propertyStock,
        oracle,
        valuation,
    };
    Coordinator::BundleBuildStats stats;
    const std::vector<Coordinator::CandidateBundle> candidates = Coordinator::buildCandidateBundles(
        context, static_cast<qint32>(pKnownActor - knowledge.units().data()), stats);
    if (candidates.empty())
    {
        return QStringLiteral("no-candidates");
    }
    if (stats.missingUnits != 0 || stats.invalidCount != 0)
    {
        return QStringLiteral("builder-dropped-candidates");
    }
    return checkExposure(candidates);
}
}

QVariantMap FocusFireProbe::run(QObject*, const QVariantMap & arguments)
{
    const QString mapPath = arguments.value(QStringLiteral("mapPath")).toString();
    const QString failure = mapPath.isEmpty() ? QStringLiteral("missing-map-path") : focusFireExposure(mapPath);
    return {
        {QStringLiteral("ok"), failure.isEmpty()},
        {QStringLiteral("failures"), failure.isEmpty() ? QStringList() : QStringList{failure}},
    };
}

namespace
{
[[maybe_unused]] const bool REGISTERED = AiArenaTestSupport::registerOperation(
    QStringLiteral("focusFireExposure"), FocusFireProbe::run);
}
