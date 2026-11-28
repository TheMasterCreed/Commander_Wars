#include "ai/coordinator/bundlebuilder.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

#include <QPoint>
#include <QSet>

#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/survivorshot.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/replyfactsbuilder.h"
#include "ai/coreai.h"

#include "coreengine/gameconsole.h"
#include "coreengine/interpreter.h"
#include "coreengine/memorymanagement.h"

#include "game/building.h"
#include "game/co.h"
#include "game/gameaction.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/unitpathfindingsystem.h"
#include "game/victoryrule.h"

#include "gameinput/markedfielddata.h"

namespace
{
    using Coordinator::MilliFunds;
    using Coordinator::NO_SHOT_DISTANCE;
    using Coordinator::Side;
    using Coordinator::TilePoint;

    constexpr qint32 NO_MOVEMENT_COST = 0;
    // getAllNodePointsFast wants an exclusive cost bound.
    constexpr qint32 NODE_COST_BOUND_MARGIN = 1;
    // GameAction::canBePerformed asks whether the action targets an empty field.
    constexpr bool EMPTY_FIELD_ACTION = false;

    QString candidateActionId(
        Coordinator::PlanBundleKind kind)
    {
        switch (kind)
        {
            case Coordinator::PlanBundleKind::Wait:
            case Coordinator::PlanBundleKind::Move:
                return QString::fromLatin1(
                    CoreAI::ACTION_WAIT);
            case Coordinator::PlanBundleKind::Fire:
            case Coordinator::PlanBundleKind::MoveAndFire:
                return QString::fromLatin1(
                    CoreAI::ACTION_FIRE);
            case Coordinator::PlanBundleKind::Capture:
            case Coordinator::PlanBundleKind::MoveAndCapture:
                return QString::fromLatin1(
                    CoreAI::ACTION_CAPTURE);
            case Coordinator::PlanBundleKind::Service:
            case Coordinator::PlanBundleKind::MoveAndService:
            case Coordinator::PlanBundleKind::Compound:
                break;
        }
        return QStringLiteral("NONE");
    }

    struct ShotSource
    {
        std::int32_t unitIndex{Coordinator::NO_UNIT};
        Unit* pUnit{nullptr};
        TilePoint tile{Coordinator::INVALID_TILE};
        std::int32_t hpSteps{0};
        std::int32_t movementPoints{0};
        std::int32_t minRange{1};
        std::int32_t maxRange{1};
        bool canMoveAndFire{false};
        Side side{Side::Bystander};
    };

    struct VictimOverride
    {
        std::int32_t unitIndex{Coordinator::NO_UNIT};
        // Zero means the unit is gone, so it neither shoots nor can be shot at.
        std::int32_t hpSteps{0};
    };

    struct MoveOption
    {
        TilePoint destination{Coordinator::INVALID_TILE};
        std::vector<TilePoint> path;
        std::vector<QPoint> enginePath;
        qint32 costs{NO_MOVEMENT_COST};
        Unit *vacatingUnit{nullptr};
        Unit *clearingUnit{nullptr};
        std::vector<std::int32_t> requiredKilledEnemyIds;
        std::vector<std::int32_t> requiredKilledKnowledgeIndices;
    };

    // Nothing refuels between now and the shot, so the fuel left after moving caps next turn's reach.
    std::int32_t movedMovementPoints(const Coordinator::KnownUnit & known, qint32 spentCosts)
    {
        if (known.fuel < 0)
        {
            return known.movementPoints;
        }
        return std::min(known.movementPoints, std::max(known.fuel - spentCosts, 0));
    }

    bool mapHasPositionFireRangeModifier(GameMap & map)
    {
        const qint32 playerCount = map.getPlayerCount();
        for (qint32 player = 0; player < playerCount; ++player)
        {
            Player* pPlayer = map.getPlayer(player);
            if (pPlayer == nullptr)
            {
                continue;
            }
            for (qint32 slot = 0; slot < Coordinator::MobilityEpoch::CO_SLOT_COUNT; ++slot)
            {
                if (pPlayer->getCO(static_cast<quint8>(slot)) != nullptr)
                {
                    return true;
                }
            }
        }
        Interpreter* pInterpreter = Interpreter::getInstance();
        if (pInterpreter == nullptr)
        {
            return true;
        }
        static const QString FIRERANGE_FUNCTION = QStringLiteral("getFirerangeModifier");
        QSet<QString> checked;
        const qint32 width = map.getMapWidth();
        const qint32 height = map.getMapHeight();
        for (qint32 y = 0; y < height; ++y)
        {
            for (qint32 x = 0; x < width; ++x)
            {
                Terrain* pTerrain = map.getTerrain(x, y);
                if (pTerrain == nullptr)
                {
                    continue;
                }
                const QString terrainId = pTerrain->getTerrainID();
                if (checked.contains(terrainId))
                {
                    continue;
                }
                checked.insert(terrainId);
                if (pInterpreter->exists(terrainId, FIRERANGE_FUNCTION))
                {
                    return true;
                }
            }
        }
        return false;
    }

    class CandidateBuilder
    {
    public:
        CandidateBuilder(const Coordinator::BundleBuildContext & context, Coordinator::BundleBuildStats & stats)
            : m_context(context),
              m_stats(stats),
              m_pCache(context.pCache != nullptr ? context.pCache : &m_ownedCache)
        {
            ensureCacheInitialized();
        }

        std::vector<Coordinator::CandidateBundle> build(std::int32_t actorUnitIndex,
                                                        std::optional<std::size_t> & todayCount);
        void appendBreakthrough(std::int32_t actorUnitIndex,
                                std::span<const Coordinator::KillableEnemyTarget> killableEnemies,
                                std::vector<Coordinator::CandidateBundle> & result,
                                std::optional<std::size_t> & todayCount);

    private:
        struct CaptureTraceFacts
        {
            std::int32_t propertyIndex{Coordinator::NO_PROPERTY_INDEX};
            std::int32_t stockColumn{Coordinator::NO_STOCK_COLUMN};
            std::int32_t existingPoints{0};
            std::int32_t rate{0};
        };

        void ensureCacheInitialized();
        void syncReachCacheEpoch();
        const std::vector<std::int32_t> & sideUnits(Side side) const;
        Unit* liveUnit(std::int32_t unitIndex);
        MilliFunds replacementCost(std::int32_t unitIndex, Unit & unit);
        bool describeShooter(std::int32_t unitIndex, std::int32_t hpSteps, const TilePoint & tile, ShotSource & shooter);
        const Coordinator::MobilityCostGrid* mobilityGridFor(std::int32_t unitIndex);
        const Coordinator::DistanceField & reachFrom(const ShotSource & shooter,
                                                     const Coordinator::MobilityCostGrid & grid);
        std::int32_t estimatedShotSteps(const ShotSource & shooter, Unit & victim, std::int32_t victimHpSteps,
                                        std::int32_t distance);
        std::vector<MilliFunds> nextShotSpan(const ShotSource & shooter, const VictimOverride & changed,
                                             std::int32_t excludedVictimIndex);
        std::vector<MilliFunds> enemyShotSpan(const ShotSource & actor, const VictimOverride & damaged);
        MilliFunds targetBestShot(std::int32_t targetUnitIndex, std::int32_t hpSteps, std::int32_t excludedVictimIndex);
        std::vector<MoveOption> collectMoveOptions(const ShotSource & actor, UnitPathFindingSystem & pfs,
                                                   std::vector<MoveOption> & followOptions);
        void addCandidate(Coordinator::CandidateBundle && candidate,
                          std::vector<Coordinator::CandidateBundle> & result,
                          const CaptureTraceFacts* pCapture = nullptr);
        void addPositionalCandidates(const ShotSource & actor, const std::vector<MoveOption> & options,
                                     std::vector<Coordinator::CandidateBundle> & result);
        void addFireCandidates(const ShotSource & actor, const std::vector<MoveOption> & options,
                               std::vector<Coordinator::CandidateBundle> & result);
        void addFireCandidatesFrom(const ShotSource & actor, const MoveOption & option,
                                   std::vector<Coordinator::CandidateBundle> & result);
        void addCaptureCandidates(const ShotSource & actor, const std::vector<MoveOption> & options,
                                  std::vector<Coordinator::CandidateBundle> & result);
        std::shared_ptr<const Coordinator::SurvivorWeaponFacts> survivorWeapon(int actorIndex);
        std::shared_ptr<const Coordinator::SurvivorWeaponFacts> m_survivorWeapon;
        const Coordinator::PropertyFacts* propertyAt(const TilePoint & tile) const;
        Coordinator::CandidateBundle makeCandidate(const ShotSource & actor, const MoveOption & option,
                                                   std::int32_t actorHpStepsAfter, const VictimOverride & damaged);

        const Coordinator::BundleBuildContext & m_context;
        Coordinator::BundleBuildStats & m_stats;
        Coordinator::BundleBuildCache m_ownedCache;
        Coordinator::BundleBuildCache* m_pCache{nullptr};
        std::vector<MilliFunds> m_actorShotsAtOrigin;
        std::vector<MilliFunds> m_enemyShotsAtOrigin;
        std::int32_t m_generationIndex{0};
        std::int32_t m_actorEngineUnitId{Coordinator::NO_UNIT};
        Unit* m_pActor{nullptr};
        bool m_actorOnActiveFriendlyProduction{false};
    };

    void CandidateBuilder::ensureCacheInitialized()
    {
        syncReachCacheEpoch();
        if (m_pCache->initialized)
        {
            return;
        }
        const std::vector<Coordinator::KnownUnit> & units = m_context.knowledge.units();
        for (std::size_t i = 0; i < units.size(); ++i)
        {
            const Coordinator::KnownUnit & unit = units[i];
            if (unit.carrier != Coordinator::KnownUnit::NO_CARRIER)
            {
                continue;
            }
            const Side side = Coordinator::sideOf(m_context.knowledge.relation(unit.ownerId));
            if (side == Side::Ours)
            {
                m_pCache->oursUnits.push_back(static_cast<std::int32_t>(i));
            }
            else if (side == Side::Theirs)
            {
                m_pCache->theirsUnits.push_back(static_cast<std::int32_t>(i));
            }
        }
        m_pCache->breakableTerrainTiles.clear();
        for (std::int32_t y = 0; y < m_context.knowledge.height(); ++y)
        {
            for (std::int32_t x = 0; x < m_context.knowledge.width(); ++x)
            {
                Terrain * pTerrain = m_context.map.getTerrain(x, y);
                if (pTerrain == nullptr || pTerrain->getUnit() != nullptr)
                    continue;
                Building * pBld = pTerrain->getBuilding();
                const bool attackableBuilding = pBld != nullptr && pBld->getHp() > 0 &&
                    (pBld->getOwner() == nullptr ||
                     m_context.knowledge.relation(pBld->getOwner()->getPlayerID()) == Coordinator::Relation::Enemy);
                if ((pTerrain->getHp() > 0 && pBld == nullptr) || attackableBuilding)
                    m_pCache->breakableTerrainTiles.push_back(TilePoint{x, y});
            }
        }
        m_pCache->positionFireRangeFixed = !mapHasPositionFireRangeModifier(m_context.map);
        m_pCache->initialized = true;
    }

    void CandidateBuilder::syncReachCacheEpoch()
    {
        const qint32 epochChanges = m_context.mobility.epochChangeCount();
        if (m_pCache->mobilityEpochChanges != epochChanges)
        {
            m_pCache->mobilityGrids.clear();
            m_pCache->reachFields.clear();
            m_pCache->mobilityEpochChanges = epochChanges;
        }
    }

    const std::vector<std::int32_t> & CandidateBuilder::sideUnits(Side side) const
    {
        static const std::vector<std::int32_t> EMPTY;
        if (side == Side::Ours)
        {
            return m_pCache->oursUnits;
        }
        if (side == Side::Theirs)
        {
            return m_pCache->theirsUnits;
        }
        return EMPTY;
    }

    Unit* CandidateBuilder::liveUnit(std::int32_t unitIndex)
    {
        const std::vector<Coordinator::KnownUnit> & units = m_context.knowledge.units();
        if (unitIndex < 0 || unitIndex >= static_cast<std::int32_t>(units.size()))
        {
            return nullptr;
        }
        return Coordinator::liveUnitFor(m_context.map, units[static_cast<std::size_t>(unitIndex)]);
    }

    MilliFunds CandidateBuilder::replacementCost(std::int32_t unitIndex, Unit & unit)
    {
        const auto known = m_pCache->replacementCosts.find(unitIndex);
        if (known != m_pCache->replacementCosts.end())
        {
            return known->second;
        }
        const MilliFunds cost = Coordinator::toMilliFunds(unit.getUnitCosts());
        m_pCache->replacementCosts.emplace(unitIndex, cost);
        return cost;
    }

    bool CandidateBuilder::describeShooter(std::int32_t unitIndex, std::int32_t hpSteps, const TilePoint & tile,
                                           ShotSource & shooter)
    {
        Unit* pUnit = liveUnit(unitIndex);
        if (pUnit == nullptr)
        {
            ++m_stats.missingUnits;
            AI_CONSOLE_PRINT("Coordinator::buildCandidateBundles() no live unit for index " +
                             QString::number(unitIndex), GameConsole::eWARNING);
            return false;
        }
        const Coordinator::KnownUnit & known = m_context.knowledge.units()[static_cast<std::size_t>(unitIndex)];
        shooter = ShotSource{
            .unitIndex = unitIndex,
            .pUnit = pUnit,
            .tile = tile,
            .hpSteps = hpSteps,
            .movementPoints = known.movementPoints,
            .minRange = known.minRange,
            .maxRange = known.maxRange,
            .canMoveAndFire = known.canMoveAndFire,
            .side = Coordinator::sideOf(m_context.knowledge.relation(known.ownerId)),
        };
        return true;
    }

    const Coordinator::MobilityCostGrid* CandidateBuilder::mobilityGridFor(std::int32_t unitIndex)
    {
        const Coordinator::KnownUnit & unit = m_context.knowledge.units()[static_cast<std::size_t>(unitIndex)];
        const std::pair<qint32, QString> key{unit.ownerId, unit.unitId};
        const auto cached = m_pCache->mobilityGrids.find(key);
        if (cached != m_pCache->mobilityGrids.end())
        {
            return cached->second;
        }
        Player* pOwner = m_context.map.getPlayer(unit.ownerId);
        if (pOwner == nullptr)
        {
            AI_CONSOLE_PRINT("Coordinator::buildCandidateBundles() no owner " + QString::number(unit.ownerId),
                             GameConsole::eERROR);
            return nullptr;
        }
        const Coordinator::MobilityCostGrid & grid = m_context.mobility.grid(m_context.map, *pOwner, unit.unitId);
        syncReachCacheEpoch();
        m_pCache->mobilityGrids.emplace(key, &grid);
        return &grid;
    }

    const Coordinator::DistanceField & CandidateBuilder::reachFrom(const ShotSource & shooter,
                                                                   const Coordinator::MobilityCostGrid & grid)
    {
        const std::int32_t maxDistance = std::max(shooter.movementPoints, 0);
        const Coordinator::BundleBuildCache::ReachKey key{&grid, shooter.tile.x, shooter.tile.y, maxDistance};
        const auto known = m_pCache->reachFields.find(key);
        if (known != m_pCache->reachFields.end())
        {
            return known->second;
        }
        ++m_stats.distanceFieldBuilds;
        Coordinator::DistanceField field = Coordinator::DistanceField::build(
            grid, {shooter.tile}, Coordinator::FieldExpansion::FromSources, maxDistance);
        return m_pCache->reachFields.emplace(key, std::move(field)).first->second;
    }

    std::int32_t CandidateBuilder::estimatedShotSteps(const ShotSource & shooter, Unit & victim,
                                                      std::int32_t victimHpSteps, std::int32_t distance)
    {
        return Coordinator::damageStepsFrom(
            victimHpSteps, Coordinator::scaledBaseDamage(m_context.oracle, *shooter.pUnit, shooter.hpSteps,
                                                         shooter.minRange, shooter.maxRange, victim, distance));
    }

    std::vector<MilliFunds> CandidateBuilder::nextShotSpan(const ShotSource & shooter, const VictimOverride & changed,
                                                           std::int32_t excludedVictimIndex)
    {
        std::vector<MilliFunds> span;
        if (shooter.hpSteps <= 0 || shooter.side == Side::Bystander)
        {
            return span;
        }
        const Side victimSide = Coordinator::opposingSide(shooter.side);
        const std::vector<std::int32_t> & targets = sideUnits(victimSide);
        if (targets.empty())
        {
            return span;
        }
        const std::vector<Coordinator::KnownUnit> & units = m_context.knowledge.units();
        const Coordinator::MobilityCostGrid* pGrid = nullptr;
        bool gridResolved = false;
        const Coordinator::DistanceField* pReach = nullptr;
        const std::int32_t maxReachDistance =
            std::max(shooter.movementPoints, 0) + std::max(shooter.maxRange, 0);
        for (const std::int32_t targetIndex : targets)
        {
            // The attacker's own safety is already the exposure term, so its shot is not priced here too.
            if (targetIndex == excludedVictimIndex)
            {
                continue;
            }
            const Coordinator::KnownUnit & victim = units[static_cast<std::size_t>(targetIndex)];
            std::int32_t victimHpSteps = victim.hpRounded;
            if (targetIndex == changed.unitIndex)
            {
                victimHpSteps = changed.hpSteps;
            }
            if (victimHpSteps <= 0)
            {
                continue;
            }
            const TilePoint victimTile{victim.x, victim.y};
            std::int32_t distance = NO_SHOT_DISTANCE;
            if (!shooter.canMoveAndFire)
            {
                distance = Coordinator::stationaryShotDistance(shooter.tile, victimTile, shooter.minRange,
                                                               shooter.maxRange);
            }
            else
            {
                if (!gridResolved)
                {
                    pGrid = mobilityGridFor(shooter.unitIndex);
                    gridResolved = true;
                }
                if (pGrid == nullptr)
                {
                    continue;
                }
                if (!pGrid->hasZeroCost() &&
                    Coordinator::manhattanDistance(shooter.tile, victimTile) > maxReachDistance)
                {
                    continue;
                }
                if (pReach == nullptr)
                {
                    pReach = &reachFrom(shooter, *pGrid);
                }
                distance = Coordinator::movedShotDistance(*pReach, shooter.movementPoints, shooter.minRange,
                                                          shooter.maxRange, victimTile);
            }
            if (distance == NO_SHOT_DISTANCE)
            {
                continue;
            }
            Unit* pVictim = liveUnit(targetIndex);
            if (pVictim == nullptr)
            {
                ++m_stats.missingUnits;
                continue;
            }
            const std::int32_t steps = estimatedShotSteps(shooter, *pVictim, victimHpSteps, distance);
            if (steps <= 0)
            {
                continue;
            }
            span.push_back(Coordinator::bookDamage(replacementCost(targetIndex, *pVictim), steps));
        }
        return span;
    }

    // Hp lives in whole steps here, so damageStepsFrom already caps a shot at what the actor still has.
    std::vector<MilliFunds> CandidateBuilder::enemyShotSpan(const ShotSource & actor, const VictimOverride & damaged)
    {
        std::vector<MilliFunds> span;
        if (actor.hpSteps <= 0)
        {
            return span;
        }
        const MilliFunds actorCost = replacementCost(actor.unitIndex, *actor.pUnit);
        Coordinator::forEachEnemyShot(
            m_context.knowledge.units(), m_context.enemyReach, actor.tile, damaged.unitIndex, damaged.hpSteps,
            [&](const Coordinator::EnemyShot & shot)
            {
                ShotSource enemy;
                if (!describeShooter(shot.unitIndex, shot.hpSteps, shot.tile, enemy) ||
                    shot.distance == NO_SHOT_DISTANCE)
                {
                    return;
                }
                const std::int32_t steps = estimatedShotSteps(enemy, *actor.pUnit, actor.hpSteps, shot.distance);
                if (steps > 0)
                {
                    span.push_back(Coordinator::bookDamage(actorCost, steps));
                }
            });
        return span;
    }

    MilliFunds CandidateBuilder::targetBestShot(std::int32_t targetUnitIndex, std::int32_t hpSteps,
                                                std::int32_t excludedVictimIndex)
    {
        if (hpSteps <= 0)
        {
            return 0;
        }
        const Coordinator::BundleBuildCache::ShotKey key{targetUnitIndex, hpSteps, excludedVictimIndex};
        const auto known = m_pCache->targetBestShots.find(key);
        if (known != m_pCache->targetBestShots.end())
        {
            return known->second;
        }
        const Coordinator::KnownUnit & target = m_context.knowledge.units()[static_cast<std::size_t>(targetUnitIndex)];
        ShotSource shooter;
        MilliFunds best = 0;
        if (describeShooter(targetUnitIndex, hpSteps, TilePoint{target.x, target.y}, shooter))
        {
            best = Coordinator::bestShot(nextShotSpan(shooter, VictimOverride{}, excludedVictimIndex));
        }
        m_pCache->targetBestShots.emplace(key, best);
        return best;
    }

    std::vector<MoveOption> CandidateBuilder::collectMoveOptions(const ShotSource & actor, UnitPathFindingSystem & pfs,
                                                                 std::vector<MoveOption> & followOptions)
    {
        std::vector<MoveOption> options;
        MoveOption stay;
        stay.destination = actor.tile;
        stay.path.push_back(actor.tile);
        stay.enginePath.push_back(QPoint(actor.tile.x, actor.tile.y));
        options.push_back(std::move(stay));
        for (const QPoint & node : pfs.getAllNodePointsFast(actor.movementPoints + NODE_COST_BOUND_MARGIN))
        {
            const TilePoint destination{node.x(), node.y()};
            if (destination == actor.tile)
            {
                continue;
            }
            Terrain* pTerrain = m_context.map.getTerrain(destination.x, destination.y);
            if (pTerrain == nullptr)
            {
                continue;
            }
            const bool follows = pTerrain->getUnit() != nullptr;
            if (follows && !Coordinator::followTileOffered(m_context.followTiles, destination, actor.tile))
            {
                continue;
            }
            MoveOption option;
            option.destination = destination;
            option.enginePath = pfs.getPathFast(destination.x, destination.y);
            if (option.enginePath.empty())
            {
                continue;
            }
            option.costs = pfs.getCosts(option.enginePath);
            option.path = Coordinator::pathFromEngineOrder(option.enginePath);
            if (follows)
            {
                option.vacatingUnit = pTerrain->getUnit();
                followOptions.push_back(std::move(option));
            }
            else
            {
                options.push_back(std::move(option));
            }
        }
        return options;
    }

    // Ranges stay the origin's, since a destination's own range hooks would need the unit standing there.
    Coordinator::CandidateBundle CandidateBuilder::makeCandidate(const ShotSource & actor, const MoveOption & option,
                                                                 std::int32_t actorHpStepsAfter,
                                                                 const VictimOverride & damaged)
    {
        Coordinator::CandidateBundle candidate;
        candidate.bundle.unitId = actor.unitIndex;
        candidate.bundle.origin = actor.tile;
        candidate.bundle.destination = option.destination;
        candidate.bundle.path = option.path;
        candidate.movementCost = option.costs;
        candidate.vacatesActiveFriendlyProduction =
            m_actorOnActiveFriendlyProduction &&
            option.destination != actor.tile;
        candidate.requiredKilledEnemyIds = option.requiredKilledEnemyIds;
        candidate.requiredKilledKnowledgeIndices = option.requiredKilledKnowledgeIndices;
        candidate.actor = Coordinator::ActorFacts{
            .replacementCost = replacementCost(actor.unitIndex, *actor.pUnit),
            .hpSteps = actor.hpSteps,
        };
        candidate.actorNextShotsAtOrigin = m_actorShotsAtOrigin;
        candidate.enemyShotsOnActorAtOrigin = m_enemyShotsAtOrigin;
        if (actorHpStepsAfter > 0)
        {
            const Coordinator::KnownUnit & known = m_context.knowledge.units()[static_cast<std::size_t>(actor.unitIndex)];
            ShotSource moved = actor;
            moved.tile = option.destination;
            moved.hpSteps = actorHpStepsAfter;
            moved.movementPoints = movedMovementPoints(known, option.costs);
            candidate.actorNextShotsAtDestination = nextShotSpan(moved, damaged, Coordinator::NO_UNIT);
            candidate.enemyShotsOnActorAtDestination = enemyShotSpan(moved, damaged);
        }
        return candidate;
    }

    // Isolated lethal counters can disappear when an earlier ally weakens the target.
    // Keep one weapon table and mobility grid per such actor, shared by all its fire orders.
    std::shared_ptr<const Coordinator::SurvivorWeaponFacts> CandidateBuilder::survivorWeapon(int actorIndex)
    {
        if (m_survivorWeapon)
            return m_survivorWeapon;
        const auto &knowledge = m_context.knowledge;
        const auto &actor = knowledge.units().at(actorIndex);
        auto facts = std::make_shared<Coordinator::SurvivorWeaponFacts>();
        facts->movement = actor.movementPoints;
        facts->fuel = actor.fuel;
        facts->ammo = {m_pActor->getMaxAmmo1() < 0 ? -1 : actor.ammo1,
                       m_pActor->getMaxAmmo2() < 0 ? -1 : actor.ammo2};
        facts->ignoresCollision = actor.ignoresUnitCollision;
        if (actor.canMoveAndFire)
            facts->grid = m_context.mobility.grid(m_context.map, m_context.player, actor.unitId);
        const int range = std::min<int>(actor.maxRange, knowledge.width() + knowledge.height() - 2);
        for (int i = 0; i < static_cast<int>(knowledge.units().size()); ++i)
        {
            const auto &enemy = knowledge.units()[i];
            if (enemy.carrier != Coordinator::KnownUnit::NO_CARRIER || knowledge.relation(enemy.ownerId) != Coordinator::Relation::Enemy)
                continue;
            auto *victim = liveUnit(i);
            if (!victim)
                continue;
            Coordinator::SurvivorTarget target{i, {enemy.x, enemy.y}, enemy.hpRounded, replacementCost(i, *victim)};
            for (int d = 0; d <= range; ++d)
                target.baseByRange.push_back({m_context.oracle.bestSlotDamage(*m_pActor, *victim, d, actor.minRange,
                                                                              actor.maxRange, true, false),
                                              m_context.oracle.bestSlotDamage(*m_pActor, *victim, d, actor.minRange,
                                                                              actor.maxRange, false, true)});
            facts->targets.push_back(std::move(target));
        }
        m_survivorWeapon = std::move(facts);
        return m_survivorWeapon;
    }

    void CandidateBuilder::addCandidate(
        Coordinator::CandidateBundle && candidate,
        std::vector<Coordinator::CandidateBundle> & result,
        const CaptureTraceFacts* pCapture)
    {
        if (m_context.pTrace != nullptr)
        {
            candidate.generationIndex = m_generationIndex;
            ++m_generationIndex;
        }
        candidate.valuation = Coordinator::valueBundle(candidate.bundle, candidate.actor,
                                                       Coordinator::positionFacts(candidate), m_context.valuation);
        ++m_stats.candidateCount;
        const bool valid = candidate.valuation.valid;
        if (m_context.pTrace != nullptr &&
            m_context.pTrace->candidateDetailsEnabled())
        {
            Coordinator::TilePoint target = Coordinator::INVALID_TILE;
            std::int32_t targetUnit = Coordinator::NO_UNIT;
            std::int32_t targetEngineUnit =
                Coordinator::NO_UNIT;
            const Coordinator::BundleComponent* pComponent =
                candidate.bundle.components.size() == 1
                    ? &candidate.bundle.components.front()
                    : nullptr;
            if (pComponent != nullptr)
            {
                if (pComponent->kind == Coordinator::ComponentKind::Fire)
                {
                    targetUnit = pComponent->fire.targetUnitId;
                    if (Coordinator::isTerrainTargetId(targetUnit))
                    {
                        target = pComponent->fire.targetTile != Coordinator::INVALID_TILE
                            ? pComponent->fire.targetTile
                            : Coordinator::decodeTerrainTargetTile(targetUnit);
                        targetEngineUnit = targetUnit;
                    }
                    else if (targetUnit >= 0 &&
                             targetUnit < static_cast<std::int32_t>(
                                              m_context.knowledge.units().size()))
                    {
                        const Coordinator::KnownUnit & known =
                            m_context.knowledge.units()[static_cast<std::size_t>(
                                targetUnit)];
                        target = Coordinator::TilePoint{known.x, known.y};
                        Unit* pTarget = liveUnit(targetUnit);
                        if (pTarget != nullptr)
                        {
                            targetEngineUnit =
                                pTarget->getUniqueID();
                        }
                    }
                }
                else if (pComponent->kind ==
                         Coordinator::ComponentKind::Capture)
                {
                    target = candidate.bundle.destination;
                }
            }
            const Coordinator::EconomicDelta & ledger =
                candidate.valuation.ledger;
            const Coordinator::ContinuationDelta & continuation =
                candidate.valuation.continuation;
            const Coordinator::PlanBundleKind kind =
                Coordinator::planBundleKindOf(
                    candidate.bundle);
            QString fields =
                QStringLiteral(
                    "actor=%1 actorKnowledge=%2 generated=%3 returned=%4 kind=%5 actionId=%6 origin=%7 destination=%8 target=%9 targetUnitKnowledge=%10 path=%11 movementCost=%12 validity=%13 ledgerIncome=%14 ledgerEnemyCapital=%15 ledgerFriendlyCapital=%16 ledgerActionCost=%17 ledgerResourceCost=%18 ledgerScriptedCapital=%19 ledgerRevaluation=%20 continuationTargetRemoved=%21 continuationPartner=%22 continuationProperty=%23 continuationRepositioning=%24 continuationExposure=%25 economicValue=%26 targetUnitEngine=%27 plannable=%28 support=%29")
                    .arg(m_actorEngineUnitId)
                    .arg(candidate.bundle.unitId)
                    .arg(candidate.generationIndex)
                    .arg(valid ? static_cast<std::int32_t>(result.size()) : -1)
                    .arg(Coordinator::traceBundleKind(kind))
                    .arg(candidateActionId(kind))
                    .arg(Coordinator::traceTile(candidate.bundle.origin))
                    .arg(Coordinator::traceTile(candidate.bundle.destination))
                    .arg(Coordinator::traceTile(target))
                    .arg(targetUnit)
                    .arg(Coordinator::tracePath(candidate.bundle.path))
                    .arg(candidate.movementCost)
                    .arg(Coordinator::traceBool(valid))
                    .arg(ledger.income)
                    .arg(ledger.enemyCapital)
                    .arg(ledger.friendlyCapital)
                    .arg(ledger.actionCost)
                    .arg(ledger.resourceCost)
                    .arg(ledger.scriptedCapital)
                    .arg(ledger.revaluation)
                    .arg(continuation.targetContinuationRemoved)
                    .arg(continuation.partnerContinuation)
                    .arg(continuation.propertyContinuation)
                    .arg(continuation.repositioning)
                    .arg(continuation.exposure)
                    .arg(candidate.valuation.value().economicValue)
                    .arg(targetEngineUnit)
                    .arg(
                        valid
                            ? QStringLiteral("PENDING")
                            : QStringLiteral("false"))
                    .arg(
                        valid
                            ? QStringLiteral(
                                  "DEFERRED_TO_ASSIGNMENT")
                            : QStringLiteral(
                                  "INVALID_VALUATION"));
            if (pCapture != nullptr && pComponent != nullptr)
            {
                fields +=
                    QStringLiteral(
                        " property=%1 stockColumn=%2 ownerBefore=%3 ownerAfter=%4 incomeOurs=%5 incomeEnemy=%6 existingCapturePoints=%7 captureRate=%8 turnsUntilOwned=%9")
                        .arg(pCapture->propertyIndex)
                        .arg(pCapture->stockColumn)
                        .arg(Coordinator::traceOwnerSign(pComponent->capture.ownerBefore))
                        .arg(Coordinator::traceOwnerSign(pComponent->capture.ownerAfter))
                        .arg(pComponent->capture.income.oursPerTurn)
                        .arg(pComponent->capture.income.enemyPerTurn)
                        .arg(pCapture->existingPoints)
                        .arg(pCapture->rate)
                        .arg(pComponent->capture.turnsUntilOwned);
            }
            if (pComponent != nullptr &&
                pComponent->kind == Coordinator::ComponentKind::Fire)
            {
                fields +=
                    QStringLiteral(
                        " reservationTargetHpSteps=%1 reservationRequestedDamageSteps=%2")
                        .arg(pComponent->fire.targetHpSteps)
                        .arg(pComponent->fire.damageSteps);
            }
            m_context.pTrace->record(
                QStringLiteral("CANDIDATE_GENERATED"), fields);
        }
        if (!candidate.valuation.valid)
        {
            ++m_stats.invalidCount;
            AI_CONSOLE_PRINT("Coordinator::buildCandidateBundles() dropped an invalid candidate for unit " +
                             QString::number(candidate.bundle.unitId), GameConsole::eWARNING);
            return;
        }
        if (m_context.pReplyFacts != nullptr)
        {
            candidate.replyFacts =
                m_context.pReplyFacts->factsFor(candidate.bundle.unitId, *m_pActor, candidate.bundle.destination);
            if (Coordinator::hpAfterAction(candidate.bundle, candidate.actor.hpSteps).hpSteps <= 0)
            {
                auto conditional = std::make_shared<Coordinator::ReplyFacts>(*candidate.replyFacts);
                conditional->survivorWeapon = survivorWeapon(candidate.bundle.unitId);
                candidate.replyFacts = std::move(conditional);
            }
            m_context.pReplyFacts->countCandidate();
        }
        result.push_back(std::move(candidate));
    }

    void CandidateBuilder::addPositionalCandidates(const ShotSource & actor, const std::vector<MoveOption> & options,
                                                   std::vector<Coordinator::CandidateBundle> & result)
    {
        for (const MoveOption & option : options)
        {
            if (option.clearingUnit != nullptr)
            {
                spGameAction pAction = MemoryManagement::create<GameAction>(CoreAI::ACTION_WAIT, &m_context.map);
                pAction->setTarget(QPoint(actor.tile.x, actor.tile.y));
                pAction->setMovepath(option.enginePath, option.costs);
                if (!pAction->canBePerformedAfterClearing(CoreAI::ACTION_WAIT, m_context.player, *option.clearingUnit))
                    continue;
            }
            addCandidate(makeCandidate(actor, option, actor.hpSteps, VictimOverride{}), result);
        }
    }

    // Terrain and building targets are out of scope until a later commit prices structures.
    void CandidateBuilder::addFireCandidatesFrom(const ShotSource & actor, const MoveOption & option,
                                                 std::vector<Coordinator::CandidateBundle> & result)
    {
        if (!actor.pUnit->hasWeapons())
        {
            return;
        }
        if (m_pCache->positionFireRangeFixed)
        {
            const std::vector<std::int32_t> & targets = sideUnits(Coordinator::opposingSide(actor.side));
            const std::vector<Coordinator::KnownUnit> & units = m_context.knowledge.units();
            const std::int32_t maxBound = std::max(actor.maxRange, 1);
            bool anyInRange = false;
            for (const std::int32_t targetIndex : targets)
            {
                const Coordinator::KnownUnit & victim = units[static_cast<std::size_t>(targetIndex)];
                if (victim.hpRounded > 0 &&
                    Coordinator::manhattanDistance(option.destination, TilePoint{victim.x, victim.y}) <= maxBound)
                {
                    anyInRange = true;
                    break;
                }
            }
            if (!anyInRange)
            {
                for (const TilePoint terrainTile : m_pCache->breakableTerrainTiles)
                {
                    if (Coordinator::manhattanDistance(option.destination, terrainTile) <= maxBound)
                    {
                        anyInRange = true;
                        break;
                    }
                }
            }
            if (!anyInRange)
            {
                return;
            }
        }
        spGameAction pAction = MemoryManagement::create<GameAction>(CoreAI::ACTION_FIRE, &m_context.map);
        pAction->setTarget(QPoint(actor.tile.x, actor.tile.y));
        pAction->setMovepath(option.enginePath, option.costs);
        const bool eligible = option.clearingUnit
            ? pAction->canBePerformedAfterClearing(CoreAI::ACTION_FIRE, m_context.player, *option.clearingUnit)
            : (option.vacatingUnit
                ? pAction->canBePerformedAfterVacating(CoreAI::ACTION_FIRE, m_context.player, *option.vacatingUnit)
                : pAction->canBePerformed(CoreAI::ACTION_FIRE, EMPTY_FIELD_ACTION, &m_context.player));
        if (!eligible)
        {
            return;
        }
        spMarkedFieldData pFields = pAction->getMarkedFieldStepData();
        if (pFields == nullptr)
        {
            return;
        }
        const QVector<QPoint> targets = *pFields->getPoints();
        for (const QPoint & target : targets)
        {
            const Coordinator::KnownUnit* pKnownTarget = m_context.knowledge.unitAt(target.x(), target.y());
            if (pKnownTarget == nullptr)
            {
                Terrain * pTargetTerrain = m_context.map.getTerrain(target.x(), target.y());
                if (pTargetTerrain == nullptr || pTargetTerrain->getUnit() != nullptr)
                {
                    continue;
                }
                Building * pTargetBld = pTargetTerrain->getBuilding();
                const bool attackableBuilding = pTargetBld != nullptr && pTargetBld->getHp() > 0 &&
                    (pTargetBld->getOwner() == nullptr ||
                     m_context.knowledge.relation(pTargetBld->getOwner()->getPlayerID()) == Coordinator::Relation::Enemy);
                const qint32 targetRawHp = attackableBuilding ? pTargetBld->getHp() : pTargetTerrain->getHp();
                if (targetRawHp <= 0 || (!attackableBuilding && pTargetBld != nullptr))
                {
                    continue;
                }
                const QRectF envReport = Interpreter::getInstance()->doFunction(
                    CoreAI::ACTION_FIRE,
                    QStringLiteral("calcBattleDamage"),
                    QJSValueList({
                        GameMap::getMapJsThis(&m_context.map),
                        JsThis::getJsThis(pAction.get()),
                        QJSValue(target.x()),
                        QJSValue(target.y()),
                        QJSValue(static_cast<qint32>(GameEnums::LuckDamageMode_Average)),
                    })).toVariant().toRectF();
                if (envReport.x() <= 0.0)
                {
                    continue;
                }
                const TilePoint targetTile{target.x(), target.y()};
                const double terrainRealHp = std::min<double>(
                    Coordinator::UNIT_HP_STEPS,
                    static_cast<double>(targetRawHp) / Coordinator::TERRAIN_HP_PER_STEP);
                const std::int32_t targetHpSteps = Coordinator::ceilHpSteps(terrainRealHp);
                const std::int32_t dmgSteps = Coordinator::damageStepsFrom(terrainRealHp, envReport.x());
                if (targetHpSteps <= 0 || dmgSteps <= 0)
                {
                    continue;
                }
                const Coordinator::FireFacts fire{
                    .targetUnitId = Coordinator::encodeTerrainTargetId(targetTile),
                    .targetTile = targetTile,
                    .targetReplacementCost = 0,
                    .targetHpSteps = targetHpSteps,
                    .damageSteps = dmgSteps,
                    .counterSteps = 0,
                    .targetBestShotBefore = 0,
                    .targetBestShotAfter = 0,
                    .targetRealHp = terrainRealHp,
                    .targetAmmo = {-1, -1},
                    .isolated = Coordinator::OwnFireReport{envReport.x(), -1.0, static_cast<int>(envReport.y()), -1},
                };
                Coordinator::CandidateBundle candidate = makeCandidate(actor, option, actor.hpSteps, VictimOverride{});
                candidate.bundle.components.push_back(Coordinator::fireComponent(fire));
                addCandidate(std::move(candidate), result);
                continue;
            }
            if (Coordinator::sideOf(m_context.knowledge.relation(pKnownTarget->ownerId)) !=
                Coordinator::opposingSide(actor.side))
            {
                continue;
            }
            const std::int32_t targetIndex = static_cast<std::int32_t>(pKnownTarget - m_context.knowledge.units().data());
            if (std::ranges::find(option.requiredKilledKnowledgeIndices, targetIndex) !=
                option.requiredKilledKnowledgeIndices.end())
            {
                continue;
            }
            Unit* pTarget = liveUnit(targetIndex);
            if (pTarget == nullptr || pTarget == option.clearingUnit)
            {
                if (pTarget == nullptr)
                    ++m_stats.missingUnits;
                continue;
            }
            const Coordinator::DamageEstimate estimate =
                m_context.oracle.estimate(actor.unitIndex, *actor.pUnit, option.destination,
                                          targetIndex, *pTarget, TilePoint{target.x(), target.y()});
            if (!estimate.possible)
            {
                continue;
            }
            const std::int32_t targetHpSteps = pTarget->getHpRounded();
            const std::int32_t dealt = std::min(estimate.damageSteps, targetHpSteps);
            std::int32_t taken = 0;
            if (dealt < targetHpSteps)
            {
                taken = std::min(estimate.counterSteps, actor.hpSteps);
            }
            const std::int32_t targetHpStepsAfter = targetHpSteps - dealt;
            const Coordinator::FireFacts fire{
                .targetUnitId = targetIndex,
                .targetReplacementCost = replacementCost(targetIndex, *pTarget),
                .targetHpSteps = targetHpSteps,
                .damageSteps = estimate.damageSteps,
                .counterSteps = estimate.counterSteps,
                .targetBestShotBefore = targetBestShot(targetIndex, targetHpSteps, actor.unitIndex),
                .targetBestShotAfter = targetBestShot(targetIndex, targetHpStepsAfter, actor.unitIndex),
                .targetRealHp = pTarget->getHp(),
                .targetAmmo = {pTarget->getMaxAmmo1() < 0 ? -1 : pTarget->getAmmo1(),
                               pTarget->getMaxAmmo2() < 0 ? -1 : pTarget->getAmmo2()},
                .isolated = estimate.report,
            };
            Coordinator::CandidateBundle candidate = makeCandidate(actor, option, actor.hpSteps - taken,
                                                                   VictimOverride{targetIndex, targetHpStepsAfter});
            candidate.bundle.components.push_back(Coordinator::fireComponent(fire));
            addCandidate(std::move(candidate), result);
        }
    }

    void CandidateBuilder::addFireCandidates(const ShotSource & actor, const std::vector<MoveOption> & options,
                                             std::vector<Coordinator::CandidateBundle> & result)
    {
        for (const MoveOption & option : options)
        {
            if (option.destination != actor.tile && !actor.canMoveAndFire)
            {
                continue;
            }
            addFireCandidatesFrom(actor, option, result);
        }
    }

    const Coordinator::PropertyFacts* CandidateBuilder::propertyAt(const TilePoint & tile) const
    {
        const std::int32_t index = m_context.propertyStock.propertyIndexAt(tile);
        if (index == Coordinator::NO_PROPERTY_INDEX)
        {
            return nullptr;
        }
        return &m_context.properties[static_cast<std::size_t>(index)];
    }

    void CandidateBuilder::addCaptureCandidates(const ShotSource & actor, const std::vector<MoveOption> & options,
                                                std::vector<Coordinator::CandidateBundle> & result)
    {
        if (!actor.pUnit->canCapture())
        {
            return;
        }
        for (const MoveOption & option : options)
        {
            const Coordinator::PropertyFacts* pFacts = propertyAt(option.destination);
            if (pFacts == nullptr || !pFacts->capturable)
            {
                continue;
            }
            spGameAction pAction = MemoryManagement::create<GameAction>(CoreAI::ACTION_CAPTURE, &m_context.map);
            pAction->setTarget(QPoint(actor.tile.x, actor.tile.y));
            pAction->setMovepath(option.enginePath, option.costs);
            const bool eligible = option.clearingUnit
                ? pAction->canBePerformedAfterClearing(CoreAI::ACTION_CAPTURE, m_context.player, *option.clearingUnit)
                : (option.vacatingUnit
                    ? pAction->canBePerformedAfterVacating(CoreAI::ACTION_CAPTURE, m_context.player, *option.vacatingUnit)
                    : pAction->canBePerformed(CoreAI::ACTION_CAPTURE, EMPTY_FIELD_ACTION, &m_context.player));
            if (!eligible)
            {
                continue;
            }
            std::int32_t existingPoints = 0;
            if (pFacts->capturerIndex == actor.unitIndex)
            {
                existingPoints = pFacts->capturePoints;
            }
            const qint32 rate = actor.pUnit->getCaptureRate(QPoint(option.destination.x, option.destination.y));
            const Coordinator::CaptureFacts capture{
                .income = Coordinator::captureIncome(*pFacts),
                .ownerBefore = Coordinator::captureOwnerBefore(pFacts->ownerId,
                                                               m_context.knowledge.relation(pFacts->ownerId)),
                .ownerAfter = Coordinator::OwnerSign::Ours,
                // This bundle's own capture action already lands, so its points are spent before the count starts.
                .turnsUntilOwned = Coordinator::captureTurnsFor(existingPoints + rate, rate, Unit::MAX_CAPTURE_POINTS),
            };
            Coordinator::CandidateBundle candidate = makeCandidate(actor, option, actor.hpSteps, VictimOverride{});
            candidate.bundle.components.push_back(Coordinator::captureComponent(capture));
            const CaptureTraceFacts traceFacts{
                .propertyIndex =
                    m_context.propertyStock.propertyIndexAt(option.destination),
                .stockColumn =
                    m_context.propertyStock.columnSlotAt(option.destination),
                .existingPoints = existingPoints,
                .rate = rate,
            };
            addCandidate(std::move(candidate), result, &traceFacts);
        }
    }

    std::vector<Coordinator::CandidateBundle> CandidateBuilder::build(std::int32_t actorUnitIndex,
                                                                      std::optional<std::size_t> & todayCount)
    {
        std::vector<Coordinator::CandidateBundle> result;
        const std::int64_t oracleCallsBefore = m_context.oracle.callCount();
        const std::int64_t oracleHitsBefore = m_context.oracle.hitCount();
        const std::vector<Coordinator::KnownUnit> & units = m_context.knowledge.units();
        if (actorUnitIndex < 0 || actorUnitIndex >= static_cast<std::int32_t>(units.size()))
        {
            AI_CONSOLE_PRINT("Coordinator::buildCandidateBundles() unknown actor index " +
                             QString::number(actorUnitIndex), GameConsole::eERROR);
            return result;
        }
        const Coordinator::KnownUnit & known = units[static_cast<std::size_t>(actorUnitIndex)];
        if (known.hasMoved || m_context.knowledge.relation(known.ownerId) != Coordinator::Relation::Own)
        {
            AI_CONSOLE_PRINT("Coordinator::buildCandidateBundles() actor " + QString::number(actorUnitIndex) +
                             " is not an unmoved own unit", GameConsole::eERROR);
            return result;
        }
        ShotSource actor;
        if (!describeShooter(actorUnitIndex, known.hpRounded, TilePoint{known.x, known.y}, actor))
        {
            return result;
        }
        m_actorEngineUnitId = actor.pUnit->getUniqueID();
        m_pActor = actor.pUnit;
        const Coordinator::PropertyFacts* pOriginProperty =
            propertyAt(actor.tile);
        m_actorOnActiveFriendlyProduction =
            pOriginProperty != nullptr &&
            pOriginProperty->canProduce &&
            m_context.knowledge.relation(
                pOriginProperty->ownerId) ==
                Coordinator::Relation::Own;
        m_actorShotsAtOrigin = nextShotSpan(actor, VictimOverride{}, Coordinator::NO_UNIT);
        m_enemyShotsAtOrigin = enemyShotSpan(actor, VictimOverride{});
        UnitPathFindingSystem pfs(&m_context.map, actor.pUnit, &m_context.player);
        pfs.explore();
        std::vector<MoveOption> followOptions;
        const std::vector<MoveOption> options = collectMoveOptions(actor, pfs, followOptions);
        todayCount = Coordinator::appendFollowersBehind(
            result,
            [&](std::vector<Coordinator::CandidateBundle> & candidates)
            {
                addPositionalCandidates(actor, options, candidates);
                addFireCandidates(actor, options, candidates);
                addCaptureCandidates(actor, options, candidates);
            },
            // The master enforces the prior occupant's departure and the engine revalidates the order live.
            [&](std::vector<Coordinator::CandidateBundle> & candidates)
            {
                addPositionalCandidates(actor, followOptions, candidates);
                addFireCandidates(actor, followOptions, candidates);
                addCaptureCandidates(actor, followOptions, candidates);
            });
        m_stats.oracleCalls += m_context.oracle.callCount() - oracleCallsBefore;
        m_stats.oracleHits += m_context.oracle.hitCount() - oracleHitsBefore;
        return result;
    }

    void CandidateBuilder::appendBreakthrough(std::int32_t actorUnitIndex,
                                              std::span<const Coordinator::KillableEnemyTarget> killableEnemies,
                                              std::vector<Coordinator::CandidateBundle> & result,
                                              std::optional<std::size_t> & todayCount)
    {
        if (killableEnemies.empty())
            return;
        m_generationIndex = 0;
        for (const Coordinator::CandidateBundle & existing : result)
        {
            if (existing.generationIndex >= m_generationIndex)
                m_generationIndex = existing.generationIndex + 1;
        }
        const std::vector<Coordinator::KnownUnit> & units = m_context.knowledge.units();
        if (actorUnitIndex < 0 || actorUnitIndex >= static_cast<std::int32_t>(units.size()))
            return;
        const Coordinator::KnownUnit & known = units[static_cast<std::size_t>(actorUnitIndex)];
        if (known.hasMoved || known.movementPoints <= 0 || known.ignoresUnitCollision ||
            m_context.knowledge.relation(known.ownerId) != Coordinator::Relation::Own)
            return;
        ShotSource actor;
        if (!describeShooter(actorUnitIndex, known.hpRounded, TilePoint{known.x, known.y}, actor))
            return;
        const Coordinator::MobilityCostGrid* pGrid = mobilityGridFor(actorUnitIndex);
        if (pGrid == nullptr)
            return;
        const Coordinator::DistanceField & reach = reachFrom(actor, *pGrid);
        std::map<std::pair<std::int32_t, std::int32_t>, Coordinator::KillableEnemyTarget> killableByTile;
        QSet<qint32> ignoredIds;
        bool anyKillableReachable = false;
        for (const auto & k : killableEnemies)
        {
            if (k.engineUnitId == Coordinator::NO_UNIT || k.knowledgeIndex < 0)
                continue;
            killableByTile.emplace(std::pair{k.tile.x, k.tile.y}, k);
            ignoredIds.insert(k.engineUnitId);
            if (!anyKillableReachable && reach.reachable(k.tile.x, k.tile.y, actor.movementPoints))
                anyKillableReachable = true;
        }
        if (!anyKillableReachable || ignoredIds.isEmpty())
            return;

        m_actorEngineUnitId = actor.pUnit->getUniqueID();
        m_pActor = actor.pUnit;
        const Coordinator::PropertyFacts* pOriginProperty = propertyAt(actor.tile);
        m_actorOnActiveFriendlyProduction =
            pOriginProperty != nullptr && pOriginProperty->canProduce &&
            m_context.knowledge.relation(pOriginProperty->ownerId) == Coordinator::Relation::Own;
        m_actorShotsAtOrigin = nextShotSpan(actor, VictimOverride{}, Coordinator::NO_UNIT);
        m_enemyShotsAtOrigin = enemyShotSpan(actor, VictimOverride{});

        UnitPathFindingSystem pfs(&m_context.map, actor.pUnit, &m_context.player);
        pfs.setIgnoredEnemyUnits(ignoredIds);
        pfs.explore();
        std::vector<MoveOption> breakthroughOptions;
        for (const QPoint & node : pfs.getAllNodePointsFast(actor.movementPoints + NODE_COST_BOUND_MARGIN))
        {
            const TilePoint destination{node.x(), node.y()};
            if (destination == actor.tile)
                continue;
            Terrain* pTerrain = m_context.map.getTerrain(destination.x, destination.y);
            if (pTerrain == nullptr)
                continue;
            const std::vector<QPoint> enginePath = pfs.getPathFast(destination.x, destination.y);
            if (enginePath.empty())
                continue;
            const std::vector<TilePoint> path = Coordinator::pathFromEngineOrder(enginePath);
            std::vector<std::int32_t> reqIds;
            std::vector<std::int32_t> reqKis;
            for (std::size_t pi = 1; pi < path.size(); ++pi)
            {
                if (const auto ktIt = killableByTile.find(std::pair{path[pi].x, path[pi].y});
                    ktIt != killableByTile.end())
                {
                    if (std::ranges::find(reqIds, ktIt->second.engineUnitId) == reqIds.end())
                    {
                        reqIds.push_back(ktIt->second.engineUnitId);
                        reqKis.push_back(ktIt->second.knowledgeIndex);
                    }
                }
            }
            if (reqIds.empty())
                continue;

            Unit* pOcc = pTerrain->getUnit();
            Unit* clearingUnit = nullptr;
            Unit* vacatingUnit = nullptr;
            if (pOcc != nullptr)
            {
                if (killableByTile.contains(std::pair{destination.x, destination.y}))
                    clearingUnit = pOcc;
                else if (pOcc != actor.pUnit && Coordinator::followTileOffered(m_context.followTiles, destination, actor.tile))
                    vacatingUnit = pOcc;
                else
                    continue;
            }
            MoveOption option;
            option.destination = destination;
            option.path = path;
            option.enginePath = enginePath;
            option.costs = pfs.getCosts(option.enginePath);
            option.vacatingUnit = vacatingUnit;
            option.clearingUnit = clearingUnit;
            option.requiredKilledEnemyIds = std::move(reqIds);
            option.requiredKilledKnowledgeIndices = std::move(reqKis);
            breakthroughOptions.push_back(std::move(option));
        }
        if (breakthroughOptions.empty())
            return;

        std::vector<MoveOption> directBreakthrough;
        std::vector<MoveOption> followBreakthrough;
        for (auto & opt : breakthroughOptions)
        {
            if (opt.vacatingUnit != nullptr)
                followBreakthrough.push_back(std::move(opt));
            else
                directBreakthrough.push_back(std::move(opt));
        }
        const std::size_t baseCount = result.size();
        addPositionalCandidates(actor, directBreakthrough, result);
        addFireCandidates(actor, directBreakthrough, result);
        addCaptureCandidates(actor, directBreakthrough, result);
        addPositionalCandidates(actor, followBreakthrough, result);
        addFireCandidates(actor, followBreakthrough, result);
        addCaptureCandidates(actor, followBreakthrough, result);
        if (result.size() > baseCount && !todayCount.has_value())
            todayCount = baseCount;
    }
}

namespace Coordinator
{
    // A tile whose unit no longer matches the snapshot is a stale index, not the unit the caller asked for.
    Unit* liveUnitFor(GameMap & map, const KnownUnit & known)
    {
        if (known.carrier != KnownUnit::NO_CARRIER)
        {
            return nullptr;
        }
        Terrain* pTerrain = map.getTerrain(known.x, known.y);
        if (pTerrain == nullptr)
        {
            return nullptr;
        }
        Unit* pUnit = pTerrain->getUnit();
        if (pUnit == nullptr || pUnit->getUnitID() != known.unitId)
        {
            return nullptr;
        }
        Player* pOwner = pUnit->getOwner();
        if (pOwner == nullptr || pOwner->getPlayerID() != known.ownerId)
        {
            return nullptr;
        }
        return pUnit;
    }

    std::int32_t resolveRemainingTurnLimit(GameMap & map)
    {
        GameRules* pRules = map.getGameRules();
        if (pRules == nullptr)
        {
            return NO_TURN_LIMIT;
        }
        std::int32_t tightest = NO_TURN_LIMIT;
        const std::array<QString, 3> turnRules{
            TURN_LIMIT_VICTORY_RULE,
            QStringLiteral("VICTORYRULE_TURNLIMIT_CAPTURE_RACE"),
            QStringLiteral("VICTORYRULE_TURNLIMIT_SCORE_RACE"),
        };
        for (const QString & ruleId : turnRules)
        {
            VictoryRule* pRule = pRules->getVictoryRule(ruleId);
            if (pRule == nullptr)
                continue;
            const std::int32_t rem = remainingTurnLimit(pRule->getRuleValue(TURN_LIMIT_RULE_ITEM), map.getCurrentDay());
            if (rem != NO_TURN_LIMIT && (tightest == NO_TURN_LIMIT || rem < tightest))
                tightest = rem;
        }
        return tightest;
    }

    ValuationContext makeValuationContext(GameMap & map)
    {
        return ValuationContext{resolveMapHorizon(
            map.getMapWidth(), map.getMapHeight(), resolveRemainingTurnLimit(map))};
    }

    std::vector<CandidateBundle> buildCandidateBundles(const BundleBuildContext & context, std::int32_t actorUnitIndex,
                                                       BundleBuildStats & stats,
                                                       std::optional<std::size_t>* pTodayCount)
    {
        CandidateBuilder builder(context, stats);
        std::optional<std::size_t> todayCount;
        std::vector<CandidateBundle> candidates = builder.build(actorUnitIndex, todayCount);
        if (pTodayCount != nullptr)
        {
            *pTodayCount = todayCount;
        }
        return candidates;
    }

    void appendKillBreakthroughCandidates(const BundleBuildContext & context,
                                          std::span<const KillableEnemyTarget> killableEnemies,
                                          std::int32_t actorUnitIndex,
                                          std::vector<CandidateBundle> & candidates,
                                          std::optional<std::size_t>* pTodayCount,
                                          BundleBuildStats & stats)
    {
        if (killableEnemies.empty())
            return;
        CandidateBuilder builder(context, stats);
        std::optional<std::size_t> todayCount = pTodayCount != nullptr ? *pTodayCount : std::nullopt;
        builder.appendBreakthrough(actorUnitIndex, killableEnemies, candidates, todayCount);
        if (pTodayCount != nullptr)
            *pTodayCount = todayCount;
    }
}
