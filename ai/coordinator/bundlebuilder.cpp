#include "ai/coordinator/bundlebuilder.h"

#include <algorithm>
#include <map>
#include <utility>

#include <QPoint>

#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/decisiontrace.h"
#include "ai/coreai.h"

#include "coreengine/gameconsole.h"
#include "coreengine/memorymanagement.h"

#include "game/building.h"
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

    QString ownerSignName(Coordinator::OwnerSign owner)
    {
        switch (owner)
        {
            case Coordinator::OwnerSign::Enemy:
                return QStringLiteral("ENEMY");
            case Coordinator::OwnerSign::Neutral:
                return QStringLiteral("NEUTRAL");
            case Coordinator::OwnerSign::Ours:
                return QStringLiteral("OURS");
        }
        return QStringLiteral("UNKNOWN");
    }

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

    class CandidateBuilder
    {
    public:
        CandidateBuilder(const Coordinator::BundleBuildContext & context, Coordinator::BundleBuildStats & stats)
            : m_context(context),
              m_stats(stats)
        {
        }

        std::vector<Coordinator::CandidateBundle> build(std::int32_t actorUnitIndex);

    private:
        struct CaptureTraceFacts
        {
            std::int32_t propertyIndex{Coordinator::NO_PROPERTY_INDEX};
            std::int32_t stockColumn{Coordinator::NO_STOCK_COLUMN};
            std::int32_t existingPoints{0};
            std::int32_t rate{0};
        };

        struct TileKey
        {
            std::int32_t unitIndex{Coordinator::NO_UNIT};
            std::int32_t x{0};
            std::int32_t y{0};

            friend constexpr auto operator<=>(const TileKey &, const TileKey &) = default;
        };

        struct ShotKey
        {
            std::int32_t unitIndex{Coordinator::NO_UNIT};
            std::int32_t hpSteps{0};
            std::int32_t excludedVictimIndex{Coordinator::NO_UNIT};

            friend constexpr auto operator<=>(const ShotKey &, const ShotKey &) = default;
        };

        Unit* liveUnit(std::int32_t unitIndex);
        MilliFunds replacementCost(std::int32_t unitIndex, Unit & unit);
        bool describeShooter(std::int32_t unitIndex, std::int32_t hpSteps, const TilePoint & tile, ShotSource & shooter);
        const Coordinator::DistanceField & reachFrom(const ShotSource & shooter);
        std::int32_t shotDistance(const ShotSource & shooter, const TilePoint & target);
        std::int32_t estimatedShotSteps(const ShotSource & shooter, Unit & victim, std::int32_t victimHpSteps,
                                        std::int32_t distance);
        std::vector<MilliFunds> nextShotSpan(const ShotSource & shooter, const VictimOverride & changed,
                                             std::int32_t excludedVictimIndex);
        std::vector<MilliFunds> enemyShotSpan(const ShotSource & actor, const VictimOverride & damaged);
        MilliFunds targetBestShot(std::int32_t targetUnitIndex, std::int32_t hpSteps, std::int32_t excludedVictimIndex);
        std::vector<MoveOption> collectMoveOptions(const ShotSource & actor, UnitPathFindingSystem & pfs);
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
        const Coordinator::PropertyFacts* propertyAt(const TilePoint & tile) const;
        Coordinator::CandidateBundle makeCandidate(const ShotSource & actor, const MoveOption & option,
                                                   std::int32_t actorHpStepsAfter, const VictimOverride & damaged);

        const Coordinator::BundleBuildContext & m_context;
        Coordinator::BundleBuildStats & m_stats;
        std::map<std::int32_t, MilliFunds> m_replacementCosts;
        std::map<TileKey, Coordinator::DistanceField> m_reachFields;
        std::map<ShotKey, MilliFunds> m_targetBestShots;
        std::vector<MilliFunds> m_actorShotsAtOrigin;
        std::vector<MilliFunds> m_enemyShotsAtOrigin;
        std::int32_t m_generationIndex{0};
        std::int32_t m_actorEngineUnitId{Coordinator::NO_UNIT};
        bool m_actorOnActiveFriendlyProduction{false};
    };

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
        const auto known = m_replacementCosts.find(unitIndex);
        if (known != m_replacementCosts.end())
        {
            return known->second;
        }
        const MilliFunds cost = Coordinator::toMilliFunds(unit.getUnitCosts());
        m_replacementCosts.emplace(unitIndex, cost);
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

    const Coordinator::DistanceField & CandidateBuilder::reachFrom(const ShotSource & shooter)
    {
        const TileKey key{shooter.unitIndex, shooter.tile.x, shooter.tile.y};
        const auto known = m_reachFields.find(key);
        if (known != m_reachFields.end())
        {
            return known->second;
        }
        ++m_stats.distanceFieldBuilds;
        const Coordinator::KnownUnit & unit = m_context.knowledge.units()[static_cast<std::size_t>(shooter.unitIndex)];
        Player* pOwner = m_context.map.getPlayer(unit.ownerId);
        if (pOwner == nullptr)
        {
            AI_CONSOLE_PRINT("Coordinator::buildCandidateBundles() no owner " + QString::number(unit.ownerId),
                             GameConsole::eERROR);
            const Coordinator::MobilityCostGrid blocked(m_context.knowledge.width(), m_context.knowledge.height());
            return m_reachFields
                .emplace(key, Coordinator::DistanceField::build(blocked, {}, Coordinator::FieldExpansion::FromSources))
                .first->second;
        }
        const Coordinator::MobilityCostGrid & grid = m_context.mobility.grid(m_context.map, *pOwner, unit.unitId);
        Coordinator::DistanceField field = Coordinator::DistanceField::build(grid, {shooter.tile},
                                                                             Coordinator::FieldExpansion::FromSources);
        return m_reachFields.emplace(key, std::move(field)).first->second;
    }

    std::int32_t CandidateBuilder::shotDistance(const ShotSource & shooter, const TilePoint & target)
    {
        if (!shooter.canMoveAndFire)
        {
            return Coordinator::stationaryShotDistance(shooter.tile, target, shooter.minRange, shooter.maxRange);
        }
        return Coordinator::movedShotDistance(reachFrom(shooter), shooter.movementPoints, shooter.minRange,
                                              shooter.maxRange, target);
    }

    // Next turn terrain is unknown, so every span entry is base damage scaled by the shooter's hp.
    std::int32_t CandidateBuilder::estimatedShotSteps(const ShotSource & shooter, Unit & victim,
                                                      std::int32_t victimHpSteps, std::int32_t distance)
    {
        const double baseDamage = m_context.oracle.baseDamageAgainst(*shooter.pUnit, victim, distance,
                                                                     shooter.minRange, shooter.maxRange);
        const double scaled = baseDamage * shooter.hpSteps / Coordinator::UNIT_HP_STEPS;
        return Coordinator::damageStepsFrom(victimHpSteps, scaled);
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
        const std::vector<Coordinator::KnownUnit> & units = m_context.knowledge.units();
        for (std::size_t i = 0; i < units.size(); ++i)
        {
            const Coordinator::KnownUnit & victim = units[i];
            // The attacker's own safety is already the exposure term, so its shot is not priced here too.
            if (static_cast<std::int32_t>(i) == excludedVictimIndex)
            {
                continue;
            }
            if (victim.carrier != Coordinator::KnownUnit::NO_CARRIER ||
                Coordinator::sideOf(m_context.knowledge.relation(victim.ownerId)) != victimSide)
            {
                continue;
            }
            std::int32_t victimHpSteps = victim.hpRounded;
            if (static_cast<std::int32_t>(i) == changed.unitIndex)
            {
                victimHpSteps = changed.hpSteps;
            }
            if (victimHpSteps <= 0)
            {
                continue;
            }
            const std::int32_t distance = shotDistance(shooter, TilePoint{victim.x, victim.y});
            if (distance == NO_SHOT_DISTANCE)
            {
                continue;
            }
            Unit* pVictim = liveUnit(static_cast<std::int32_t>(i));
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
            span.push_back(Coordinator::bookDamage(replacementCost(static_cast<std::int32_t>(i), *pVictim), steps));
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
        const std::vector<Coordinator::KnownUnit> & units = m_context.knowledge.units();
        for (const std::int32_t slot : m_context.enemyReach.attackerSlotsAt(actor.tile.x, actor.tile.y))
        {
            const Coordinator::AttackerSpec & spec = m_context.enemyReach.attacker(slot);
            std::int32_t enemyHpSteps = units[static_cast<std::size_t>(spec.unitIndex)].hpRounded;
            if (spec.unitIndex == damaged.unitIndex)
            {
                enemyHpSteps = damaged.hpSteps;
            }
            if (enemyHpSteps <= 0)
            {
                continue;
            }
            ShotSource enemy;
            if (!describeShooter(spec.unitIndex, enemyHpSteps, TilePoint{spec.x, spec.y}, enemy))
            {
                continue;
            }
            std::int32_t distance = Coordinator::stationaryShotDistance(enemy.tile, actor.tile, spec.minRange,
                                                                        spec.maxRange);
            if (spec.canMoveAndFire)
            {
                distance = Coordinator::movedShotDistance(m_context.enemyReach.reach(slot), spec.movementPoints,
                                                          spec.minRange, spec.maxRange, actor.tile);
            }
            if (distance == NO_SHOT_DISTANCE)
            {
                continue;
            }
            const std::int32_t steps = estimatedShotSteps(enemy, *actor.pUnit, actor.hpSteps, distance);
            if (steps <= 0)
            {
                continue;
            }
            span.push_back(Coordinator::bookDamage(actorCost, steps));
        }
        return span;
    }

    MilliFunds CandidateBuilder::targetBestShot(std::int32_t targetUnitIndex, std::int32_t hpSteps,
                                                std::int32_t excludedVictimIndex)
    {
        if (hpSteps <= 0)
        {
            return 0;
        }
        const ShotKey key{targetUnitIndex, hpSteps, excludedVictimIndex};
        const auto known = m_targetBestShots.find(key);
        if (known != m_targetBestShots.end())
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
        m_targetBestShots.emplace(key, best);
        return best;
    }

    std::vector<MoveOption> CandidateBuilder::collectMoveOptions(const ShotSource & actor, UnitPathFindingSystem & pfs)
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
            if (pTerrain == nullptr || pTerrain->getUnit() != nullptr)
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
            options.push_back(std::move(option));
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
                    if (targetUnit >= 0 &&
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
                        .arg(ownerSignName(pComponent->capture.ownerBefore))
                        .arg(ownerSignName(pComponent->capture.ownerAfter))
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
        result.push_back(std::move(candidate));
    }

    void CandidateBuilder::addPositionalCandidates(const ShotSource & actor, const std::vector<MoveOption> & options,
                                                   std::vector<Coordinator::CandidateBundle> & result)
    {
        for (const MoveOption & option : options)
        {
            addCandidate(makeCandidate(actor, option, actor.hpSteps, VictimOverride{}), result);
        }
    }

    // Terrain and building targets are out of scope until a later commit prices structures.
    void CandidateBuilder::addFireCandidatesFrom(const ShotSource & actor, const MoveOption & option,
                                                 std::vector<Coordinator::CandidateBundle> & result)
    {
        spGameAction pAction = MemoryManagement::create<GameAction>(CoreAI::ACTION_FIRE, &m_context.map);
        pAction->setTarget(QPoint(actor.tile.x, actor.tile.y));
        pAction->setMovepath(option.enginePath, option.costs);
        if (!pAction->canBePerformed(CoreAI::ACTION_FIRE, EMPTY_FIELD_ACTION, &m_context.player))
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
            if (pKnownTarget == nullptr ||
                Coordinator::sideOf(m_context.knowledge.relation(pKnownTarget->ownerId)) !=
                    Coordinator::opposingSide(actor.side))
            {
                continue;
            }
            const std::int32_t targetIndex = static_cast<std::int32_t>(pKnownTarget - m_context.knowledge.units().data());
            Unit* pTarget = liveUnit(targetIndex);
            if (pTarget == nullptr)
            {
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
            if (!pAction->canBePerformed(CoreAI::ACTION_CAPTURE, EMPTY_FIELD_ACTION, &m_context.player))
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

    std::vector<Coordinator::CandidateBundle> CandidateBuilder::build(std::int32_t actorUnitIndex)
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
        const std::vector<MoveOption> options = collectMoveOptions(actor, pfs);
        addPositionalCandidates(actor, options, result);
        addFireCandidates(actor, options, result);
        addCaptureCandidates(actor, options, result);
        m_stats.oracleCalls += m_context.oracle.callCount() - oracleCallsBefore;
        m_stats.oracleHits += m_context.oracle.hitCount() - oracleHitsBefore;
        return result;
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
        VictoryRule* pRule = pRules->getVictoryRule(TURN_LIMIT_VICTORY_RULE);
        if (pRule == nullptr)
        {
            return NO_TURN_LIMIT;
        }
        return remainingTurnLimit(pRule->getRuleValue(TURN_LIMIT_RULE_ITEM), map.getCurrentDay());
    }

    ValuationContext makeValuationContext(GameMap & map)
    {
        return ValuationContext{resolveHorizon(resolveRemainingTurnLimit(map))};
    }

    std::vector<CandidateBundle> buildCandidateBundles(const BundleBuildContext & context, std::int32_t actorUnitIndex,
                                                       BundleBuildStats & stats)
    {
        CandidateBuilder builder(context, stats);
        return builder.build(actorUnitIndex);
    }
}
