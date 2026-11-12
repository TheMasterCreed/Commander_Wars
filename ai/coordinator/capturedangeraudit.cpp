#include "ai/coordinator/capturedangeraudit.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPoint>

#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/capturedanger.h"
#include "ai/coordinator/capturedangerdecisions.h"
#include "ai/coordinator/capturedangerrecord.h"
#include "ai/coordinator/capturedangerstream.h"
#include "ai/coordinator/damageoracle.h"

#include "coreengine/settings.h"

#include "game/building.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"

namespace
{
    namespace Record = Coordinator::CaptureDangerRecord;
    using Coordinator::CaptureReplyRule;
    using Coordinator::CaptureWalkInput;
    using Coordinator::CaptureWalkResult;
    using Coordinator::ExactStatus;
    using Coordinator::KnownUnit;
    using Coordinator::OwnerSign;
    using Coordinator::PropertyStockColumn;
    using Coordinator::ShotReport;
    using Coordinator::ShotStatus;
    using Coordinator::TerrainDefenseRule;
    using Coordinator::TilePoint;

    constexpr std::int32_t DANGER_AUDIT_SCHEMA = 6;
    constexpr auto DANGER_AUDIT_FILE = "coordinated-ai-danger.jsonl";
    constexpr double TERRAIN_LUCK = Coordinator::averageLuck(Coordinator::ENGINE_DEFAULT_LUCK, Coordinator::NO_MISFORTUNE);

    constexpr auto KEY_SCHEMA = "schema";
    constexpr auto KEY_DAY = "day";
    constexpr auto KEY_PLAYER = "player";
    constexpr auto KEY_PLAN_SEQUENCE = "planSequence";
    constexpr auto KEY_HORIZON = "horizon";
    constexpr auto KEY_POINTS_TO_CAPTURE = "pointsToCapture";
    constexpr auto KEY_TERRAIN_LUCK = "terrainLuck";
    constexpr auto KEY_RATE_BONUS_CONSTANT = "rateBonusHeldConstant";
    constexpr auto KEY_CAPTURABLE = "capturableProperties";
    constexpr auto KEY_UNSHOT = "unshotCapturableProperties";
    constexpr auto KEY_NODES = "nodes";
    constexpr auto KEY_PLAN = "plan";
    constexpr auto KEY_PROPERTY = "property";
    constexpr auto KEY_TILE = "tile";
    constexpr auto KEY_OWNER = "owner";
    constexpr auto KEY_INCOME_OURS = "incomeOurs";
    constexpr auto KEY_INCOME_ENEMY = "incomeEnemy";
    constexpr auto KEY_SHOOTERS = "shooters";
    constexpr auto KEY_ACTORS = "actors";
    constexpr auto KEY_KNOWLEDGE = "knowledge";
    constexpr auto KEY_UNIT = "unit";
    constexpr auto KEY_TYPE = "type";
    constexpr auto KEY_HP_REAL = "hpReal";
    constexpr auto KEY_DISTANCE = "distance";
    constexpr auto KEY_MUST_MOVE = "mustMove";
    constexpr auto KEY_FIRING_TILE = "firingTile";
    constexpr auto KEY_CAN_CAPTURE = "canCapture";
    constexpr auto KEY_CAPTURE_RATE = "captureRate";
    constexpr auto KEY_STOCK_CAPTURE_RATE = "stockCaptureRate";
    constexpr auto KEY_CANDIDATE_CAPTURE_RATE = "candidateCaptureRate";
    constexpr auto KEY_CAPTURE_POINTS = "capturePoints";
    constexpr auto KEY_STANDING_PROPERTY = "standingProperty";
    constexpr auto KEY_CAPTURING_HERE = "capturingHere";
    constexpr auto KEY_COULD_CAPTURE_IN_PLACE = "couldCaptureInPlace";
    constexpr auto KEY_ENEMY_STOCK_ROW = "enemyStockRow";
    constexpr auto KEY_DEFENDER_ON_PROPERTY = "defenderOnProperty";
    constexpr auto KEY_TERRAIN_STARS = "terrainStars";
    constexpr auto KEY_EXACT_STATUS = "exactStatus";
    constexpr auto KEY_EXACT_LATER_APPROXIMATE = "exactLaterRepliesApproximate";
    constexpr auto KEY_SHOT_DAMAGE = "shotDamage";
    constexpr auto KEY_WALKS = "walks";
    constexpr auto KEY_STREAM_ASSUMED = "streamAssumed";
    constexpr auto KEY_ORIGIN = "origin";
    constexpr auto KEY_KIND = "kind";
    constexpr auto KEY_STATE = "state";
    constexpr auto KEY_DESTINATION = "destination";
    constexpr auto KEY_CAPTURE = "capture";
    constexpr auto KEY_CARRIED = "carried";
    constexpr auto KEY_OMITTED = "omitted";
    constexpr auto KEY_EXACT_ESTIMATES = "exactEstimates";
    constexpr auto KEY_ENEMY_ATTACKER_ENGINE_CALLS = "enemyAttackerEngineCalls";
    constexpr auto KEY_MISSING_LOADED = "missingLoadedUnits";
    constexpr auto KEY_MISSING_ABSENT = "missingAbsentUnits";
    constexpr auto KEY_NANOS = "nanos";
    constexpr auto KEY_ALLIED_OCCUPANTS = "alliedOccupantsHeldInPlace";
    constexpr auto KEY_WALK_RATE_TILE = "walkCaptureRateTile";
    constexpr auto RATE_TILE_ORIGIN = "ORIGIN";
    constexpr auto RATE_TILE_DESTINATION = "DESTINATION";
    constexpr auto KEY_DIRECT = "direct";
    constexpr auto KEY_ON_OWN_BUILDING = "onOwnBuilding";
    constexpr auto KEY_DIRECT_SHOOTERS = "directShooters";
    constexpr auto KEY_DIRECT_FIRING_TILES = "directFiringTiles";
    constexpr auto KEY_FOCUSED_EXCEEDS_TILES = "focusedFireExceedsFiringTiles";
    constexpr auto KEY_FOCUSED_ORDER = "focusedFireOrder";
    constexpr auto ORDER_LARGEST_BASE_FIRST = "LARGEST_BASE_FIRST";
    constexpr auto SOURCE_BASE = "BASE";
    constexpr auto SOURCE_TERRAIN = "TERRAIN";
    constexpr auto SOURCE_EXACT = "EXACT";
    constexpr auto WALK_R1_BASE = "R1_BASE";
    constexpr auto WALK_R2_BASE = "R2_BASE";
    constexpr auto WALK_R1_TERRAIN = "R1_TERRAIN";
    constexpr auto WALK_R2_TERRAIN = "R2_TERRAIN";
    constexpr auto WALK_R1_EXACT = "R1_EXACT";
    constexpr auto WALK_R2_EXACT = "R2_EXACT";
    constexpr auto OMITTED_NO_LIVE_UNIT = "NO_LIVE_UNIT";
    constexpr auto OMITTED_NO_KNOWLEDGE_LINK = "NO_KNOWLEDGE_LINK";
    constexpr auto OMITTED_NO_PROPERTY = "NO_PROPERTY";

    enum class DamageSource : std::int8_t
    {
        Base,
        Terrain,
        Exact,
    };

    constexpr std::array REPLY_RULES{CaptureReplyRule::LargestShot, CaptureReplyRule::FocusedFire};
    constexpr std::array DAMAGE_SOURCES{DamageSource::Base, DamageSource::Terrain, DamageSource::Exact};

    const char* sourceName(DamageSource source)
    {
        switch (source)
        {
            case DamageSource::Base:
                return SOURCE_BASE;
            case DamageSource::Terrain:
                return SOURCE_TERRAIN;
            case DamageSource::Exact:
                break;
        }
        return SOURCE_EXACT;
    }

    const char* walkName(CaptureReplyRule rule, DamageSource source)
    {
        const bool largest = rule == CaptureReplyRule::LargestShot;
        switch (source)
        {
            case DamageSource::Base:
                return largest ? WALK_R1_BASE : WALK_R2_BASE;
            case DamageSource::Terrain:
                return largest ? WALK_R1_TERRAIN : WALK_R2_TERRAIN;
            case DamageSource::Exact:
                break;
        }
        return largest ? WALK_R1_EXACT : WALK_R2_EXACT;
    }

    Coordinator::CaptureDamageFormula damageFormula(GameEnums::DamageFormula formula)
    {
        switch (formula)
        {
            case GameEnums::DamageFormula_AdvanceWars1_3:
                return Coordinator::CaptureDamageFormula::AdvanceWars1To3;
            case GameEnums::DamageFormula_AdvanceWars4:
                break;
        }
        return Coordinator::CaptureDamageFormula::AdvanceWars4;
    }

    // A null pUnit is a reaching enemy whose live unit was not found; its shot stays unknown.
    struct Shooter
    {
        Coordinator::EnemyShot shot;
        Unit* pUnit{nullptr};
        TilePoint firingTile{Coordinator::INVALID_TILE};
        bool mustMove{false};
        bool direct{false};
        bool onOwnBuilding{false};
    };

    // Engine damage reports; base and luck use the shooter's rounded hp as the engine and the planner do.
    struct ShotDamage
    {
        ShotStatus weapon{ShotStatus::NoLiveShooter};
        double rawBase{0};
        double base{0};
        std::int32_t shooterHpSteps{0};
        ShotStatus exactStatus{ShotStatus::NoLiveShooter};
        double exact{0};
    };

    struct FiringCapacity
    {
        std::int32_t directShooters{0};
        std::int32_t freeTiles{0};
    };

    struct ActorDamage
    {
        std::vector<ShotDamage> shots;
        TerrainDefenseRule terrain;
        bool onProperty{false};
    };

    ShotReport shotReport(DamageSource source, const ActorDamage & actor, const ShotDamage & damage,
                          std::int32_t defenderHpSteps)
    {
        switch (source)
        {
            case DamageSource::Base:
                return Coordinator::positionFreeShot(damage.weapon, damage.base);
            case DamageSource::Terrain:
                return Coordinator::positionFreeShot(
                    damage.weapon, Coordinator::terrainReducedDamage(actor.terrain, damage.rawBase,
                                                                     damage.shooterHpSteps, defenderHpSteps));
            case DamageSource::Exact:
                break;
        }
        return ShotReport{damage.exactStatus, damage.exact};
    }

    std::vector<ShotReport> shotsAt(DamageSource source, const ActorDamage & actor, std::int32_t defenderHpSteps)
    {
        std::vector<ShotReport> shots;
        shots.reserve(actor.shots.size());
        for (const ShotDamage & damage : actor.shots)
        {
            shots.push_back(shotReport(source, actor, damage, defenderHpSteps));
        }
        return shots;
    }

    std::vector<ShotReport> inOrder(const std::vector<ShotReport> & shots, const std::vector<std::size_t> & order)
    {
        std::vector<ShotReport> ordered;
        ordered.reserve(order.size());
        for (const std::size_t index : order)
        {
            ordered.push_back(shots[index]);
        }
        return ordered;
    }

    QJsonArray tileRecord(const TilePoint & tile)
    {
        return QJsonArray{tile.x, tile.y};
    }

    void warnOnFailedWrite(bool written, const QString & path)
    {
        // The writer stops at its first failure, so one warning per process says all there is to say.
        static bool warned = false;
        if (!written && !warned)
        {
            warned = true;
            qWarning() << "Capture danger audit could not write" << path;
        }
    }

    class DangerAuditRecorder
    {
    public:
        explicit DangerAuditRecorder(const Coordinator::CaptureDangerAuditView & view)
            : m_view(view),
              m_oracle(view.map),
              m_horizonTurns(view.propertyStock.horizonTurns()),
              m_occupancy(plannedOccupancy())
        {
        }

        QJsonObject record();

    private:
        struct CaptureActor
        {
            std::int32_t knowledgeIndex{Coordinator::NO_UNIT};
            std::int32_t engineUnitId{Coordinator::NO_UNIT};
        };

        const KnownUnit & known(std::int32_t knowledgeIndex) const
        {
            return m_view.knowledge.units()[static_cast<std::size_t>(knowledgeIndex)];
        }

        OwnerSign ownerOf(const Coordinator::PropertyFacts & facts) const
        {
            return Coordinator::captureOwnerBefore(facts.ownerId, m_view.knowledge.relation(facts.ownerId));
        }

        Coordinator::PlannedOccupancy plannedOccupancy();
        Unit* liveUnit(std::int32_t knowledgeIndex);
        Coordinator::FiringRange firingRangeOf(const Coordinator::AttackerSpec & spec, const TilePoint & target) const;
        TilePoint firingTileFor(const Coordinator::EnemyShot & shot, const TilePoint & target) const;
        FiringCapacity firingCapacity(const std::vector<Shooter> & shooters, const TilePoint & target) const;
        bool onOwnBuilding(const KnownUnit & shooter) const;
        std::vector<Shooter> shootersAt(const TilePoint & property);
        TerrainDefenseRule terrainRule(Unit & actor, const TilePoint & property) const;
        ActorDamage damageOn(const std::vector<Shooter> & shooters, std::int32_t actorIndex, Unit & actor,
                             const TilePoint & property);
        CaptureWalkInput walkInput(Unit & actor, std::int32_t carriedPoints, std::int32_t rate) const;
        QJsonObject standingRecord(const KnownUnit & shooter, std::int32_t shooterIndex) const;
        QJsonArray shooterRecords(const std::vector<Shooter> & shooters) const;
        void addCapacity(QJsonObject & target, const std::vector<Shooter> & shooters, const TilePoint & property) const;
        void addDanger(QJsonObject & target, const ActorDamage & damage, const CaptureWalkInput & input,
                       const PropertyStockColumn* pColumn) const;
        QJsonObject nodeRecord(std::int32_t propertyIndex, const std::vector<Shooter> & shooters,
                               const std::vector<CaptureActor> & actors);
        QJsonObject planRecord(const Coordinator::PlannedAction & action);
        std::int32_t knowledgeIndexOf(std::int32_t engineUnitId) const;

        const Coordinator::CaptureDangerAuditView & m_view;
        Coordinator::DamageOracle m_oracle;
        std::int32_t m_horizonTurns{0};
        // Declared before m_occupancy, whose initializer counts into it.
        std::int32_t m_alliedOccupants{0};
        Coordinator::PlannedOccupancy m_occupancy;
        std::set<std::int32_t> m_missingLoaded;
        std::set<std::int32_t> m_missingAbsent;
    };

    Coordinator::PlannedOccupancy DangerAuditRecorder::plannedOccupancy()
    {
        std::vector<Coordinator::OccupantAt> ours;
        const std::vector<KnownUnit> & units = m_view.knowledge.units();
        for (std::size_t index = 0; index < units.size(); ++index)
        {
            const KnownUnit & unit = units[index];
            const Coordinator::Relation relation = m_view.knowledge.relation(unit.ownerId);
            if (!Coordinator::blocksEnemyFiringTile(relation, unit.carrier != KnownUnit::NO_CARRIER))
            {
                continue;
            }
            if (relation == Coordinator::Relation::Allied)
            {
                ++m_alliedOccupants;
            }
            ours.push_back(Coordinator::OccupantAt{static_cast<std::int32_t>(index), TilePoint{unit.x, unit.y}});
        }
        std::vector<Coordinator::PlannedMove> moves;
        for (std::int32_t action = 0; action < m_view.plan.actionCount(); ++action)
        {
            const Coordinator::PlannedAction & planned = m_view.plan.action(action);
            const std::int32_t mover = knowledgeIndexOf(planned.unitId);
            if (mover != Coordinator::NO_UNIT)
            {
                const KnownUnit & unit = known(mover);
                moves.push_back(Coordinator::PlannedMove{mover, TilePoint{unit.x, unit.y}, planned.destination});
            }
        }
        return Coordinator::PlannedOccupancy(m_view.knowledge.width(), m_view.knowledge.height(), ours, moves);
    }

    Unit* DangerAuditRecorder::liveUnit(std::int32_t knowledgeIndex)
    {
        const KnownUnit & unit = known(knowledgeIndex);
        Unit* pUnit = Coordinator::liveUnitFor(m_view.map, unit);
        if (pUnit == nullptr)
        {
            if (unit.carrier != KnownUnit::NO_CARRIER)
            {
                m_missingLoaded.insert(knowledgeIndex);
            }
            else
            {
                m_missingAbsent.insert(knowledgeIndex);
            }
        }
        return pUnit;
    }

    Coordinator::FiringRange DangerAuditRecorder::firingRangeOf(const Coordinator::AttackerSpec & spec,
                                                                const TilePoint & target) const
    {
        return Coordinator::FiringRange{.target = target, .minRange = spec.minRange, .maxRange = spec.maxRange};
    }

    TilePoint DangerAuditRecorder::firingTileFor(const Coordinator::EnemyShot & shot, const TilePoint & target) const
    {
        const Coordinator::AttackerSpec & spec = m_view.enemyReach.attacker(shot.slot);
        if (!spec.canMoveAndFire)
        {
            return shot.tile;
        }
        const Coordinator::DistanceField & reach = m_view.enemyReach.reach(shot.slot);
        return Coordinator::cheapestFiringTile(firingRangeOf(spec, target), m_occupancy,
                                               [&](std::int32_t x, std::int32_t y)
                                               {
                                                   return reach.reachable(x, y, spec.movementPoints)
                                                       ? reach.distance(x, y)
                                                       : Coordinator::UNREACHABLE;
                                               });
    }

    // Direct shooters each pick a tile alone, so more of them than free tiles makes R2 an upper bound.
    FiringCapacity DangerAuditRecorder::firingCapacity(const std::vector<Shooter> & shooters,
                                                       const TilePoint & target) const
    {
        FiringCapacity capacity;
        std::vector<TilePoint> tiles;
        for (const Shooter & shooter : shooters)
        {
            if (!shooter.direct)
            {
                continue;
            }
            ++capacity.directShooters;
            const Coordinator::AttackerSpec & spec = m_view.enemyReach.attacker(shooter.shot.slot);
            const Coordinator::DistanceField & reach = m_view.enemyReach.reach(shooter.shot.slot);
            Coordinator::forEachFreeFiringTile(
                firingRangeOf(spec, target), m_occupancy,
                [&](std::int32_t x, std::int32_t y)
                {
                    return reach.reachable(x, y, spec.movementPoints) ? reach.distance(x, y) : Coordinator::UNREACHABLE;
                },
                [&](const TilePoint & tile, std::int32_t) { tiles.push_back(tile); });
        }
        capacity.freeTiles = Coordinator::distinctTileCount(std::move(tiles));
        return capacity;
    }

    // building.js repairs and resupplies a unit on its owner's building at that owner's turn start.
    bool DangerAuditRecorder::onOwnBuilding(const KnownUnit & shooter) const
    {
        Terrain* pTerrain = m_view.map.getTerrain(shooter.x, shooter.y);
        Building* pBuilding = pTerrain == nullptr ? nullptr : pTerrain->getBuilding();
        return pBuilding != nullptr && pBuilding->getOwner() != nullptr &&
               pBuilding->getOwner()->getPlayerID() == shooter.ownerId;
    }

    std::vector<Shooter> DangerAuditRecorder::shootersAt(const TilePoint & property)
    {
        std::vector<Shooter> shooters;
        Coordinator::forEachEnemyShot(
            m_view.knowledge.units(), m_view.enemyReach, property, Coordinator::NO_UNIT, 0,
            [&](const Coordinator::EnemyShot & shot)
            {
                if (shot.distance == Coordinator::NO_SHOT_DISTANCE)
                {
                    return;
                }
                const Coordinator::AttackerSpec & spec = m_view.enemyReach.attacker(shot.slot);
                shooters.push_back(Shooter{
                    .shot = shot,
                    .pUnit = liveUnit(shot.unitIndex),
                    .firingTile = firingTileFor(shot, property),
                    .mustMove = Coordinator::shooterMustMove(shot.tile, property, spec.minRange, spec.maxRange),
                    .direct = spec.canMoveAndFire,
                    .onOwnBuilding = onOwnBuilding(known(shot.unitIndex)),
                });
            });
        return shooters;
    }

    // Unit::useTerrainDefense and Terrain::getBaseDefense only read scripts; CO terrain modifiers are left out on purpose.
    TerrainDefenseRule DangerAuditRecorder::terrainRule(Unit & actor, const TilePoint & property) const
    {
        TerrainDefenseRule rule;
        rule.luck = TERRAIN_LUCK;
        GameRules* pRules = m_view.map.getGameRules();
        Terrain* pTerrain = m_view.map.getTerrain(property.x, property.y);
        if (pRules == nullptr || pTerrain == nullptr)
        {
            return rule;
        }
        rule.defensePerStar = pRules->getTerrainDefense();
        rule.hpScaled = pRules->getHpDefenseReduction();
        rule.formula = damageFormula(pRules->getDamageFormula());
        if (actor.useTerrainDefense())
        {
            rule.stars = pTerrain->getBaseDefense();
        }
        return rule;
    }

    ActorDamage DangerAuditRecorder::damageOn(const std::vector<Shooter> & shooters, std::int32_t actorIndex,
                                              Unit & actor, const TilePoint & property)
    {
        const KnownUnit & unit = known(actorIndex);
        ActorDamage result;
        result.onProperty = TilePoint{unit.x, unit.y} == property;
        result.terrain = terrainRule(actor, property);
        result.shots.reserve(shooters.size());
        for (const Shooter & shooter : shooters)
        {
            const KnownUnit & enemy = known(shooter.shot.unitIndex);
            ShotDamage damage;
            damage.shooterHpSteps = shooter.shot.hpSteps;
            double withAmmo = 0;
            if (shooter.pUnit != nullptr)
            {
                damage.rawBase = m_oracle.baseDamageAgainst(*shooter.pUnit, actor, shooter.shot.distance, enemy.minRange,
                                                            enemy.maxRange);
                damage.base = Coordinator::scaledBaseDamage(m_oracle, *shooter.pUnit, shooter.shot.hpSteps,
                                                            enemy.minRange, enemy.maxRange, actor, shooter.shot.distance);
                withAmmo = damage.rawBase >= 0 ? damage.rawBase
                                               : m_oracle.baseDamageWithAmmo(*shooter.pUnit, actor, shooter.shot.distance,
                                                                             enemy.minRange, enemy.maxRange);
            }
            damage.weapon = Coordinator::shooterWeaponStatus(
                shooter.pUnit != nullptr, damage.rawBase, withAmmo,
                Coordinator::repairPending(shooter.shot.hpSteps, shooter.onOwnBuilding));
            damage.exactStatus = Coordinator::exactQueryStatus(damage.weapon, result.onProperty,
                                                               shooter.firingTile != Coordinator::INVALID_TILE);
            if (damage.exactStatus == ShotStatus::Computed)
            {
                const Coordinator::DamageEstimate estimate = m_oracle.estimate(
                    shooter.shot.unitIndex, *shooter.pUnit, shooter.firingTile, actorIndex, actor, property);
                damage.exactStatus = Coordinator::exactShotStatus(damage.exactStatus, estimate.possible);
                damage.exact = estimate.reportedDamage;
            }
            result.shots.push_back(damage);
        }
        return result;
    }

    // The rate bonus is held constant over the walk, although some COs scale it with hp.
    CaptureWalkInput DangerAuditRecorder::walkInput(Unit & actor, std::int32_t carriedPoints, std::int32_t rate) const
    {
        return CaptureWalkInput{
            .carriedPoints = carriedPoints,
            .hp = actor.getHp(),
            .rateBonus = rate - actor.getHpRounded(),
            .pointsToCapture = Unit::MAX_CAPTURE_POINTS,
            .horizonTurns = m_horizonTurns,
        };
    }

    QJsonObject DangerAuditRecorder::standingRecord(const KnownUnit & shooter, std::int32_t shooterIndex) const
    {
        const std::int32_t propertyIndex = m_view.propertyStock.propertyIndexAt(TilePoint{shooter.x, shooter.y});
        QJsonObject record{
            {KEY_CAN_CAPTURE, shooter.canCapture},
            {KEY_CAPTURE_RATE, shooter.captureRate},
            {KEY_CAPTURE_POINTS, shooter.capturePoints},
            {KEY_STANDING_PROPERTY, propertyIndex},
            {KEY_ENEMY_STOCK_ROW, m_view.propertyStock.enemyRowOf(shooterIndex)},
        };
        if (propertyIndex == Coordinator::NO_PROPERTY_INDEX)
        {
            record.insert(KEY_CAPTURING_HERE, false);
            record.insert(KEY_COULD_CAPTURE_IN_PLACE, false);
            return record;
        }
        const Coordinator::PropertyFacts & facts = m_view.properties[static_cast<std::size_t>(propertyIndex)];
        const OwnerSign owner = ownerOf(facts);
        const Coordinator::PropertyIncome income = Coordinator::captureIncome(facts);
        record.insert(KEY_OWNER, Coordinator::traceOwnerSign(owner));
        record.insert(KEY_INCOME_OURS, income.oursPerTurn);
        record.insert(KEY_INCOME_ENEMY, income.enemyPerTurn);
        record.insert(KEY_CAPTURING_HERE,
                      Coordinator::shooterCapturingHere(facts.capturerIndex, facts.capturePoints, shooterIndex));
        record.insert(KEY_COULD_CAPTURE_IN_PLACE,
                      Coordinator::shooterCouldCaptureInPlace(shooter.canCapture, facts.capturable, owner));
        return record;
    }

    QJsonArray DangerAuditRecorder::shooterRecords(const std::vector<Shooter> & shooters) const
    {
        QJsonArray records;
        for (const Shooter & shooter : shooters)
        {
            const KnownUnit & unit = known(shooter.shot.unitIndex);
            QJsonObject record = standingRecord(unit, shooter.shot.unitIndex);
            record.insert(KEY_KNOWLEDGE, shooter.shot.unitIndex);
            const bool live = shooter.pUnit != nullptr;
            record.insert(KEY_UNIT, live ? QJsonValue(shooter.pUnit->getUniqueID()) : QJsonValue());
            record.insert(KEY_TYPE, unit.unitId);
            record.insert(Record::KEY_HP, shooter.shot.hpSteps);
            record.insert(KEY_HP_REAL, live ? QJsonValue(shooter.pUnit->getHp()) : QJsonValue());
            record.insert(KEY_DIRECT, shooter.direct);
            record.insert(KEY_ON_OWN_BUILDING, shooter.onOwnBuilding);
            record.insert(KEY_TILE, tileRecord(shooter.shot.tile));
            record.insert(KEY_DISTANCE, shooter.shot.distance);
            record.insert(KEY_MUST_MOVE, shooter.mustMove);
            record.insert(KEY_FIRING_TILE, shooter.firingTile == Coordinator::INVALID_TILE
                                               ? QJsonValue()
                                               : QJsonValue(tileRecord(shooter.firingTile)));
            records.append(record);
        }
        return records;
    }

    void DangerAuditRecorder::addCapacity(QJsonObject & target, const std::vector<Shooter> & shooters,
                                          const TilePoint & property) const
    {
        const FiringCapacity capacity = firingCapacity(shooters, property);
        target.insert(KEY_DIRECT_SHOOTERS, capacity.directShooters);
        target.insert(KEY_DIRECT_FIRING_TILES, capacity.freeTiles);
        target.insert(KEY_FOCUSED_EXCEEDS_TILES, capacity.directShooters > capacity.freeTiles);
    }

    void DangerAuditRecorder::addDanger(QJsonObject & target, const ActorDamage & damage, const CaptureWalkInput & input,
                                        const PropertyStockColumn* pColumn) const
    {
        const std::int32_t startHpSteps = Coordinator::ceilHpSteps(input.hp);
        QJsonObject shotDamage;
        for (const DamageSource source : DAMAGE_SOURCES)
        {
            QJsonArray entries;
            for (const ShotReport & shot : shotsAt(source, damage, startHpSteps))
            {
                entries.append(Record::shotEntry(shot));
            }
            shotDamage.insert(sourceName(source), entries);
        }
        const ExactStatus exactStatus = Coordinator::exactStatusOf(shotsAt(DamageSource::Exact, damage, startHpSteps));
        std::vector<double> baseDamage;
        baseDamage.reserve(damage.shots.size());
        for (const ShotDamage & shot : damage.shots)
        {
            baseDamage.push_back(shot.base);
        }
        const std::vector<std::size_t> focusedOrder = Coordinator::focusedFireOrder(baseDamage);
        QJsonObject walks;
        for (const CaptureReplyRule rule : REPLY_RULES)
        {
            for (const DamageSource source : DAMAGE_SOURCES)
            {
                std::optional<CaptureWalkResult> walk;
                if (source != DamageSource::Exact || Coordinator::exactWalkKnown(exactStatus))
                {
                    walk = Coordinator::walkSource(input, rule, [&](std::int32_t hpSteps)
                    {
                        return inOrder(shotsAt(source, damage, hpSteps), focusedOrder);
                    });
                }
                walks.insert(walkName(rule, source), Record::walkRecord(walk, pColumn, m_horizonTurns));
            }
        }
        target.insert(KEY_DEFENDER_ON_PROPERTY, damage.onProperty);
        target.insert(KEY_TERRAIN_STARS, damage.terrain.stars);
        target.insert(KEY_EXACT_STATUS, QLatin1String(Record::statusName(exactStatus)));
        target.insert(KEY_FOCUSED_ORDER, QLatin1String(ORDER_LARGEST_BASE_FIRST));
        // One engine report covers every hp: defender taken damage feeds only the counter, and the hp malus reads real hp.
        target.insert(KEY_EXACT_LATER_APPROXIMATE, exactStatus == ExactStatus::Computed);
        target.insert(KEY_SHOT_DAMAGE, shotDamage);
        target.insert(KEY_WALKS, walks);
    }

    QJsonObject DangerAuditRecorder::nodeRecord(std::int32_t propertyIndex, const std::vector<Shooter> & shooters,
                                                const std::vector<CaptureActor> & actors)
    {
        const Coordinator::PropertyFacts & facts = m_view.properties[static_cast<std::size_t>(propertyIndex)];
        const TilePoint tile{facts.x, facts.y};
        const Coordinator::PropertyIncome income = Coordinator::captureIncome(facts);
        QJsonArray actorRecords;
        for (const CaptureActor & actor : actors)
        {
            const KnownUnit & unit = known(actor.knowledgeIndex);
            QJsonObject record{
                {KEY_KNOWLEDGE, actor.knowledgeIndex},
                {KEY_UNIT, actor.engineUnitId},
                {KEY_TYPE, unit.unitId},
                {Record::KEY_HP, unit.hpRounded},
                {KEY_STOCK_CAPTURE_RATE, unit.captureRate},
                {KEY_WALK_RATE_TILE, QLatin1String(RATE_TILE_ORIGIN)},
            };
            Unit* pActor = liveUnit(actor.knowledgeIndex);
            if (pActor == nullptr)
            {
                record.insert(KEY_OMITTED, QLatin1String(OMITTED_NO_LIVE_UNIT));
                actorRecords.append(record);
                continue;
            }
            record.insert(KEY_HP_REAL, pActor->getHp());
            const std::int32_t carried =
                Coordinator::carriedCapturePoints(facts.capturerIndex, facts.capturePoints, actor.knowledgeIndex);
            record.insert(KEY_CARRIED, carried);
            addDanger(record, damageOn(shooters, actor.knowledgeIndex, *pActor, tile),
                      walkInput(*pActor, carried, unit.captureRate), nullptr);
            actorRecords.append(record);
        }
        QJsonObject node{
            {KEY_PROPERTY, propertyIndex},
            {KEY_TILE, tileRecord(tile)},
            {KEY_OWNER, Coordinator::traceOwnerSign(ownerOf(facts))},
            {KEY_INCOME_OURS, income.oursPerTurn},
            {KEY_INCOME_ENEMY, income.enemyPerTurn},
            {KEY_SHOOTERS, shooterRecords(shooters)},
            {KEY_ACTORS, actorRecords},
        };
        addCapacity(node, shooters, tile);
        return node;
    }

    std::int32_t DangerAuditRecorder::knowledgeIndexOf(std::int32_t engineUnitId) const
    {
        for (std::size_t index = 0; index < m_view.unitLinks.size(); ++index)
        {
            if (m_view.unitLinks[index].engineUnitId == engineUnitId)
            {
                return static_cast<std::int32_t>(index);
            }
        }
        return Coordinator::NO_UNIT;
    }

    QJsonObject DangerAuditRecorder::planRecord(const Coordinator::PlannedAction & action)
    {
        const std::int32_t actorIndex = knowledgeIndexOf(action.unitId);
        const bool captures = action.kind == Coordinator::PlanBundleKind::Capture ||
                              action.kind == Coordinator::PlanBundleKind::MoveAndCapture;
        QJsonObject record{
            {KEY_KNOWLEDGE, actorIndex},
            {KEY_UNIT, action.unitId},
            {KEY_KIND, Coordinator::traceBundleKind(action.kind)},
            {KEY_STATE, Coordinator::tracePlanActionState(action.state)},
            {KEY_DESTINATION, tileRecord(action.destination)},
        };
        if (actorIndex == Coordinator::NO_UNIT)
        {
            record.insert(KEY_OMITTED, QLatin1String(OMITTED_NO_KNOWLEDGE_LINK));
            return record;
        }
        const KnownUnit & unit = known(actorIndex);
        record.insert(KEY_TYPE, unit.unitId);
        record.insert(Record::KEY_HP, unit.hpRounded);
        record.insert(KEY_ORIGIN, tileRecord(TilePoint{unit.x, unit.y}));
        record.insert(KEY_CAPTURE_POINTS, unit.capturePoints);
        record.insert(KEY_STOCK_CAPTURE_RATE, unit.captureRate);
        if (!captures)
        {
            return record;
        }
        const std::int32_t propertyIndex = m_view.propertyStock.propertyIndexAt(action.destination);
        record.insert(KEY_PROPERTY, propertyIndex);
        if (propertyIndex == Coordinator::NO_PROPERTY_INDEX)
        {
            record.insert(KEY_OMITTED, QLatin1String(OMITTED_NO_PROPERTY));
            return record;
        }
        Unit* pActor = liveUnit(actorIndex);
        if (pActor == nullptr)
        {
            record.insert(KEY_OMITTED, QLatin1String(OMITTED_NO_LIVE_UNIT));
            return record;
        }
        record.insert(KEY_HP_REAL, pActor->getHp());
        const Coordinator::PropertyFacts & facts = m_view.properties[static_cast<std::size_t>(propertyIndex)];
        const std::int32_t carried = Coordinator::carriedCapturePoints(facts.capturerIndex, facts.capturePoints, actorIndex);
        // The danger walks use the candidate's destination rate; the stock model assumes the origin rate.
        const std::int32_t candidateRate = pActor->getCaptureRate(QPoint(action.destination.x, action.destination.y));
        record.insert(KEY_CANDIDATE_CAPTURE_RATE, candidateRate);
        const PropertyStockColumn column{
            .tile = action.destination,
            .income = Coordinator::captureIncome(facts),
            .ownerBefore = ownerOf(facts),
        };
        const std::vector<Shooter> shooters = shootersAt(action.destination);
        const std::int32_t assumedTurn = Coordinator::unrepliedDeliveredTurn(walkInput(*pActor, carried, unit.captureRate));
        QJsonObject capture{
            {KEY_CARRIED, carried},
            {KEY_OWNER, Coordinator::traceOwnerSign(column.ownerBefore)},
            {KEY_INCOME_OURS, column.income.oursPerTurn},
            {KEY_INCOME_ENEMY, column.income.enemyPerTurn},
            {KEY_HORIZON, m_horizonTurns},
            {KEY_WALK_RATE_TILE, QLatin1String(RATE_TILE_DESTINATION)},
            {KEY_SHOOTERS, shooterRecords(shooters)},
            {KEY_STREAM_ASSUMED, Coordinator::captureWalkStream(column, assumedTurn, m_horizonTurns)},
        };
        addCapacity(capture, shooters, action.destination);
        addDanger(capture, damageOn(shooters, actorIndex, *pActor, action.destination),
                  walkInput(*pActor, carried, candidateRate), &column);
        record.insert(KEY_CAPTURE, capture);
        return record;
    }

    QJsonObject DangerAuditRecorder::record()
    {
        const auto started = std::chrono::steady_clock::now();
        std::vector<CaptureActor> captureActors;
        for (const Coordinator::AssignmentActor & actor : m_view.actors)
        {
            if (known(actor.knowledgeUnitIndex).canCapture)
            {
                captureActors.push_back(CaptureActor{actor.knowledgeUnitIndex, actor.engineUnitId});
            }
        }
        QJsonArray nodes;
        std::int32_t capturable = 0;
        std::int32_t unshot = 0;
        for (std::size_t index = 0; index < m_view.properties.size(); ++index)
        {
            const Coordinator::PropertyFacts & facts = m_view.properties[index];
            const TilePoint tile{facts.x, facts.y};
            if (m_view.propertyStock.columnSlotAt(tile) == Coordinator::NO_STOCK_COLUMN ||
                ownerOf(facts) == OwnerSign::Ours)
            {
                continue;
            }
            ++capturable;
            const std::vector<Shooter> shooters = shootersAt(tile);
            if (shooters.empty())
            {
                ++unshot;
                continue;
            }
            nodes.append(nodeRecord(static_cast<std::int32_t>(index), shooters, captureActors));
        }
        QJsonArray plan;
        for (std::int32_t action = 0; action < m_view.plan.actionCount(); ++action)
        {
            plan.append(planRecord(m_view.plan.action(action)));
        }
        return QJsonObject{
            {KEY_SCHEMA, DANGER_AUDIT_SCHEMA},
            {KEY_DAY, m_view.identity.day},
            {KEY_PLAYER, m_view.identity.playerId},
            {KEY_PLAN_SEQUENCE, static_cast<qint64>(m_view.identity.planSequence)},
            {KEY_HORIZON, m_horizonTurns},
            {KEY_POINTS_TO_CAPTURE, Unit::MAX_CAPTURE_POINTS},
            {KEY_TERRAIN_LUCK, TERRAIN_LUCK},
            {KEY_RATE_BONUS_CONSTANT, true},
            {KEY_CAPTURABLE, capturable},
            {KEY_UNSHOT, unshot},
            {KEY_NODES, nodes},
            {KEY_PLAN, plan},
            {KEY_EXACT_ESTIMATES, static_cast<qint64>(m_oracle.callCount())},
            {KEY_ENEMY_ATTACKER_ENGINE_CALLS, static_cast<qint64>(m_oracle.callCount() - m_oracle.hitCount())},
            {KEY_MISSING_LOADED, static_cast<qint64>(m_missingLoaded.size())},
            {KEY_MISSING_ABSENT, static_cast<qint64>(m_missingAbsent.size())},
            {KEY_ALLIED_OCCUPANTS, m_alliedOccupants},
            {KEY_NANOS, static_cast<qint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - started).count())},
        };
    }
}

namespace Coordinator
{
    void recordCaptureDangerAudit(const CaptureDangerAuditView & view)
    {
        Settings* pSettings = Settings::getInstance();
        if (pSettings == nullptr)
        {
            return;
        }
        DangerAuditRecorder recorder(view);
        const QJsonObject record = recorder.record();
        const QString path = pSettings->getUserPath() + QLatin1String(DANGER_AUDIT_FILE);
        warnOnFailedWrite(
            appendTraceRecords(path, QStringList{QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact))}),
            path);
    }
}
