#include "ai/coordinator/capturedangeraudit.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
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

    constexpr std::int32_t DANGER_AUDIT_SCHEMA = 8;
    // A counter answers an adjacent shot.
    constexpr std::int32_t COUNTER_DISTANCE = 1;
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
    constexpr auto KEY_ACTION_FACTS = "actionFacts";
    constexpr auto ACTION_FACTS_CANDIDATE = "CANDIDATE";
    constexpr auto ACTION_FACTS_UNMATCHED = "NO_CANDIDATE";
    constexpr auto KEY_HP_AFTER_ACTION = "hpAfterAction";
    constexpr auto KEY_HP_REAL_AFTER_ACTION = "hpRealAfterAction";
    constexpr auto KEY_COUNTER_APPLIED = "counterApplied";
    constexpr auto KEY_VICTIM = "victim";
    constexpr auto KEY_VICTIM_HP = "victimHp";
    constexpr auto KEY_DESTINATION_OWN_BUILDING = "destinationOwnBuilding";
    constexpr auto KEY_ACTOR_COST = "actorCost";
    constexpr auto KEY_REPLY = "reply";
    constexpr auto KEY_RULE_R1 = "R1";
    constexpr auto KEY_RULE_R2 = "R2";
    constexpr auto KEY_RULE_P = "P";
    constexpr auto KEY_RESOLUTION = "pResolution";
    constexpr auto KEY_PRICED_HP = "pricedHp";
    constexpr auto KEY_COUNTER_BASE = "counterBase";
    constexpr auto KEY_TYPE_BASE_COST = "typeBaseCost";
    constexpr auto KEY_PLANNER_TARGET_COST = "plannerTargetCost";
    constexpr auto KEY_HP_REAL_APPROXIMATE = "hpRealAfterActionApproximate";
    constexpr auto KEY_RULE_P1 = "P1";
    constexpr auto KEY_ONE_ACTION_UNIT = "p1Unit";
    constexpr auto KEY_NET = "net";
    constexpr auto KEY_ONE_ACTION_NANOS = "p1Nanos";
    constexpr auto KEY_HP_AFTER_ACTION_PLAN = "hpAfterActionPlan";
    constexpr auto KEY_HP_REAL_AFTER_ACTION_PLAN = "hpRealAfterActionPlan";
    constexpr auto KEY_HP_REAL_PLAN_APPROXIMATE = "hpRealAfterActionPlanApproximate";
    constexpr auto KEY_COUNTER_APPLIED_PLAN = "counterAppliedPlan";
    constexpr auto KEY_COUNTER_FROM_ISOLATED_VIEW = "counterFromIsolatedView";
    constexpr auto KEY_PER_UNIT_VIEW = "perUnitRulesView";
    constexpr auto PER_UNIT_VIEW_ISOLATED = "ISOLATED_BUNDLE";
    constexpr auto KEY_ONE_ACTION_HP = "p1Hp";
    constexpr auto KEY_PLAN_KILLS = "planKills";
    constexpr auto KEY_PLAN_DAMAGED = "planDamaged";
    constexpr auto KEY_PLAN_UNMATCHED_FIRES = "planUnmatchedFires";
    constexpr auto KEY_CAPTURE_INTERRUPTION = "captureInterruption";
    constexpr auto KEY_CAPTURE_STOCK_AGREES = "captureStockAgrees";
    constexpr auto WALK_P_BASE = "P_BASE";
    constexpr auto WALK_P_TERRAIN = "P_TERRAIN";
    constexpr auto KEY_SHOOTER_LOADS = "shooterLoads";
    constexpr auto KEY_LOAD_SOURCE = "shooterLoadSource";
    constexpr auto KEY_PRICED_FIRES = "pFires";
    constexpr auto KEY_FOCUSED_FIRES = "r2Fires";
    constexpr auto KEY_FOCUSED_UNKNOWN = "r2Unknown";
    constexpr auto KEY_COUNTER_SOURCE = "counterSource";
    constexpr auto KEY_REPAIR_STEPS = "repairSteps";

    enum class DamageSource : std::int8_t
    {
        Base,
        Terrain,
        Exact,
    };

    enum class ActionFacts : std::int8_t
    {
        Candidate,
        Unmatched,
    };

    enum class ExactQuery : std::int8_t
    {
        Engine,
        Skipped,
    };

    constexpr std::array REPLY_RULES{CaptureReplyRule::LargestShot, CaptureReplyRule::FocusedFire};
    constexpr std::array DAMAGE_SOURCES{DamageSource::Base, DamageSource::Terrain, DamageSource::Exact};
    constexpr std::array PRICED_SOURCES{DamageSource::Base, DamageSource::Terrain};
    constexpr DamageSource LOAD_SOURCE = DamageSource::Terrain;

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

    const char* pricedWalkName(DamageSource source)
    {
        return source == DamageSource::Base ? WALK_P_BASE : WALK_P_TERRAIN;
    }

    const char* actionFactsName(ActionFacts facts)
    {
        return facts == ActionFacts::Candidate ? ACTION_FACTS_CANDIDATE : ACTION_FACTS_UNMATCHED;
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
        Coordinator::MilliFunds cost{0};
        std::optional<Coordinator::MilliFunds> plannerTargetCost;
        Coordinator::CaptureInterruption interruption{Coordinator::CaptureInterruption::None};
        Coordinator::MilliFunds captureCost{0};
        // Whether the in-place stream equals the enemy stock's own row weight; empty when nothing is priced.
        std::optional<bool> stockAgrees;
    };

    // Engine damage reports; base and luck use the shooter's rounded hp as the engine and the planner do.
    struct ShotDamage
    {
        ShotStatus weapon{ShotStatus::NoLiveShooter};
        double rawBase{0};
        double base{0};
        double withAmmo{0};
        std::int32_t shooterHpSteps{0};
        ShotStatus exactStatus{ShotStatus::NoLiveShooter};
        double exact{0};
        Coordinator::PricedResolution resolution{Coordinator::PricedResolution::NoLiveShooter};
        std::int32_t pricedHpSteps{0};
        double counterRawBase{Coordinator::NO_COUNTER};
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

    // Plan entries one shooter fires on in their single reply; R2 counts only entries whose R2 is known.
    struct ShooterLoad
    {
        std::int32_t pricedFires{0};
        std::int32_t focusedFires{0};
        std::int32_t focusedUnknown{0};
        std::optional<std::int32_t> oneActionUnit;
        std::int32_t oneActionHpSteps{0};
    };

    // A plan entry's reply facts, kept so P1 can run over the whole plan once every entry is known.
    struct ReplyEntry
    {
        qsizetype planSlot{0};
        std::int32_t unitId{Coordinator::NO_UNIT};
        double hp{0};
        Coordinator::MilliFunds cost{0};
        std::vector<Shooter> shooters;
        ActorDamage damage;
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

    template <typename Item>
    std::vector<Item> inOrder(const std::vector<Item> & items, const std::vector<std::size_t> & order)
    {
        std::vector<Item> ordered;
        ordered.reserve(order.size());
        for (const std::size_t index : order)
        {
            ordered.push_back(items[index]);
        }
        return ordered;
    }

    std::vector<std::size_t> focusedOrderOf(const ActorDamage & damage)
    {
        std::vector<double> baseDamage;
        baseDamage.reserve(damage.shots.size());
        for (const ShotDamage & shot : damage.shots)
        {
            baseDamage.push_back(shot.base);
        }
        return Coordinator::focusedFireOrder(baseDamage);
    }

    // P prices AMMO_GATED and REPAIR_PENDING shooters at what their own building gives them before they fire.
    double pricedDamageAt(DamageSource source, const ActorDamage & actor, const ShotDamage & damage,
                          std::int32_t shooterHpSteps, std::int32_t defenderHpSteps)
    {
        const double rawBase = Coordinator::pricedRawBase(damage.resolution, damage.rawBase, damage.withAmmo);
        if (source == DamageSource::Base)
        {
            return Coordinator::hpScaledBase(rawBase, shooterHpSteps);
        }
        return Coordinator::terrainReducedDamage(actor.terrain, rawBase, shooterHpSteps, defenderHpSteps);
    }

    double pricedDamage(DamageSource source, const ActorDamage & actor, const ShotDamage & damage,
                        std::int32_t defenderHpSteps)
    {
        return pricedDamageAt(source, actor, damage, damage.pricedHpSteps, defenderHpSteps);
    }

    std::vector<Coordinator::PricedShooter> pricedShooters(const std::vector<Shooter> & shooters,
                                                           const ActorDamage & damage,
                                                           const std::vector<std::size_t> & order)
    {
        std::vector<Coordinator::PricedShooter> priced;
        priced.reserve(order.size());
        for (const std::size_t index : order)
        {
            priced.push_back(Coordinator::PricedShooter{damage.shots[index].resolution, shooters[index].captureCost});
        }
        return priced;
    }

    // Shots and counters in focused order, from listed slot order[position].
    template <typename Run>
    auto runPriced(DamageSource source, const std::vector<Shooter> & shooters, const ActorDamage & damage,
                   const std::vector<std::size_t> & order, Run && run)
    {
        return run(pricedShooters(shooters, damage, order),
                   [&](std::size_t position, std::int32_t hpSteps)
                   {
                       return pricedDamage(source, damage, damage.shots[order[position]], hpSteps);
                   },
                   [&](std::size_t position, std::int32_t hpStepsAfter)
                   {
                       const ShotDamage & shot = damage.shots[order[position]];
                       return Coordinator::counterCost(shot.counterRawBase, hpStepsAfter, shot.pricedHpSteps,
                                                       shooters[order[position]].cost);
                   });
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
            for (const Coordinator::AssignmentActor & actor : m_view.actors)
            {
                for (const Coordinator::CandidateBundle & candidate : actor.candidates)
                {
                    for (const Coordinator::BundleComponent & component : candidate.bundle.components)
                    {
                        if (component.kind == Coordinator::ComponentKind::Fire)
                        {
                            m_plannerCosts.emplace(component.fire.targetUnitId, component.fire.targetReplacementCost);
                        }
                    }
                }
            }
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
        bool ownBuildingAt(const TilePoint & tile, std::int32_t ownerId) const;
        bool onOwnBuilding(const KnownUnit & shooter) const;
        void priceShooter(Shooter & shooter);
        std::vector<Shooter> shootersAt(const TilePoint & property, std::int32_t victimIndex = Coordinator::NO_UNIT,
                                        std::int32_t victimHpSteps = 0);
        TerrainDefenseRule terrainRule(Unit & actor, const TilePoint & property) const;
        ActorDamage damageOn(const std::vector<Shooter> & shooters, std::int32_t actorIndex, Unit & actor,
                             const TilePoint & property, ExactQuery exactQuery = ExactQuery::Engine);
        CaptureWalkInput walkInput(Unit & actor, std::int32_t carriedPoints, std::int32_t rate) const;
        QJsonObject standingRecord(const KnownUnit & shooter, std::int32_t shooterIndex) const;
        QJsonArray shooterRecords(const std::vector<Shooter> & shooters) const;
        void addCapacity(QJsonObject & target, const std::vector<Shooter> & shooters, const TilePoint & property) const;
        void addDanger(QJsonObject & target, const ActorDamage & damage, const CaptureWalkInput & input,
                       const PropertyStockColumn* pColumn) const;
        void addPricedWalks(QJsonObject & target, const std::vector<Shooter> & shooters, const ActorDamage & damage,
                            const CaptureWalkInput & input, Coordinator::MilliFunds actorCost,
                            const PropertyStockColumn & column) const;
        QJsonObject replyRecord(const std::vector<Shooter> & shooters, const ActorDamage & damage, double hp,
                                Coordinator::MilliFunds actorCost, const TilePoint & destination);
        QJsonObject nodeRecord(std::int32_t propertyIndex, const std::vector<Shooter> & shooters,
                               const std::vector<CaptureActor> & actors);
        const Coordinator::CandidateBundle* candidateFor(const Coordinator::PlannedAction & action) const;
        Coordinator::MilliFunds actorCostFor(const Coordinator::PlannedAction & action, Unit & actor) const;
        QJsonObject planRecord(const Coordinator::PlannedAction & action, qsizetype planSlot);
        void addOneActionReplies(QJsonArray & plan);
        QJsonArray shooterLoadRecords() const;
        std::int32_t knowledgeIndexOf(std::int32_t engineUnitId) const;

        const Coordinator::CaptureDangerAuditView & m_view;
        Coordinator::DamageOracle m_oracle;
        std::int32_t m_horizonTurns{0};
        // Declared before m_occupancy, whose initializer counts into it.
        std::int32_t m_alliedOccupants{0};
        Coordinator::PlannedOccupancy m_occupancy;
        std::set<std::int32_t> m_missingLoaded;
        std::set<std::int32_t> m_missingAbsent;
        std::map<std::int32_t, Coordinator::MilliFunds> m_plannerCosts;
        std::map<QString, Coordinator::MilliFunds> m_baseCosts;
        std::map<std::int32_t, ShooterLoad> m_shooterLoads;
        std::vector<ReplyEntry> m_replyEntries;
        // The plan view: one target chain filled in plan order.
        Coordinator::TargetHpChain m_planChain;
        std::int32_t m_unmatchedFires{0};
        std::int32_t m_planKills{0};
        std::int32_t m_planDamaged{0};
        std::int64_t m_oneActionNanos{0};
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

    bool DangerAuditRecorder::ownBuildingAt(const TilePoint & tile, std::int32_t ownerId) const
    {
        Terrain* pTerrain = m_view.map.getTerrain(tile.x, tile.y);
        Building* pBuilding = pTerrain == nullptr ? nullptr : pTerrain->getBuilding();
        return pBuilding != nullptr && pBuilding->getOwner() != nullptr &&
               pBuilding->getOwner()->getPlayerID() == ownerId;
    }

    // building.js repairs and resupplies a unit on its owner's building at that owner's turn start.
    bool DangerAuditRecorder::onOwnBuilding(const KnownUnit & shooter) const
    {
        return ownBuildingAt(TilePoint{shooter.x, shooter.y}, shooter.ownerId);
    }

    // Every shooter is priced at its type's base cost, one scale for all, so no enemy unit runs a script.
    void DangerAuditRecorder::priceShooter(Shooter & shooter)
    {
        const std::int32_t index = shooter.shot.unitIndex;
        const KnownUnit & unit = known(index);
        const auto planned = m_plannerCosts.find(index);
        if (planned != m_plannerCosts.end())
        {
            shooter.plannerTargetCost = planned->second;
        }
        auto base = m_baseCosts.find(unit.unitId);
        if (base == m_baseCosts.end())
        {
            // getBaseCost answers -1 for a type without a cost.
            const std::int32_t funds = std::max(Unit::getBaseCosts(unit.unitId, &m_view.map), 0);
            base = m_baseCosts.emplace(unit.unitId, Coordinator::toMilliFunds(funds)).first;
        }
        shooter.cost = base->second;
        const TilePoint tile{unit.x, unit.y};
        const std::int32_t propertyIndex = m_view.propertyStock.propertyIndexAt(tile);
        const std::int32_t slot = m_view.propertyStock.columnSlotAt(tile);
        if (propertyIndex == Coordinator::NO_PROPERTY_INDEX || slot == Coordinator::NO_STOCK_COLUMN)
        {
            return;
        }
        const Coordinator::PropertyFacts & facts = m_view.properties[static_cast<std::size_t>(propertyIndex)];
        shooter.interruption = Coordinator::captureInterruption(
            Coordinator::shooterCapturingHere(facts.capturerIndex, facts.capturePoints, index), shooter.mustMove);
        if (shooter.interruption == Coordinator::CaptureInterruption::None)
        {
            return;
        }
        const PropertyStockColumn & column = m_view.propertyStock.enemyColumn(slot);
        const std::int32_t points = m_view.propertyStock.capturePointsToCapture();
        shooter.captureCost = Coordinator::forgoneCaptureStream(column, facts.capturePoints, unit.captureRate,
                                                                shooter.interruption, points, m_horizonTurns);
        const std::int32_t row = m_view.propertyStock.enemyRowOf(index);
        if (row != Coordinator::NO_STOCK_ROW)
        {
            shooter.stockAgrees = Coordinator::captureStreamInPlace(column, facts.capturePoints, unit.captureRate, points,
                                                                    m_horizonTurns) ==
                                  m_view.propertyStock.enemyWeight(row, slot);
        }
    }

    // A victim our shot leaves alive replies at its hp after the shot; a killed one does not reply.
    std::vector<Shooter> DangerAuditRecorder::shootersAt(const TilePoint & property, std::int32_t victimIndex,
                                                         std::int32_t victimHpSteps)
    {
        std::vector<Shooter> shooters;
        Coordinator::forEachEnemyShot(
            m_view.knowledge.units(), m_view.enemyReach, property, victimIndex, victimHpSteps,
            [&](const Coordinator::EnemyShot & shot)
            {
                if (shot.distance == Coordinator::NO_SHOT_DISTANCE)
                {
                    return;
                }
                const Coordinator::AttackerSpec & spec = m_view.enemyReach.attacker(shot.slot);
                Shooter shooter{
                    .shot = shot,
                    .pUnit = liveUnit(shot.unitIndex),
                    .firingTile = firingTileFor(shot, property),
                    .mustMove = Coordinator::shooterMustMove(shot.tile, property, spec.minRange, spec.maxRange),
                    .direct = spec.canMoveAndFire,
                    .onOwnBuilding = onOwnBuilding(known(shot.unitIndex)),
                };
                priceShooter(shooter);
                shooters.push_back(shooter);
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

    // The counter is a weapon table lookup with our unit as attacker, never an engine damage call.
    ActorDamage DangerAuditRecorder::damageOn(const std::vector<Shooter> & shooters, std::int32_t actorIndex,
                                              Unit & actor, const TilePoint & property, ExactQuery exactQuery)
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
            if (shooter.pUnit != nullptr)
            {
                damage.rawBase = m_oracle.baseDamageAgainst(*shooter.pUnit, actor, shooter.shot.distance, enemy.minRange,
                                                            enemy.maxRange);
                damage.base = Coordinator::scaledBaseDamage(m_oracle, *shooter.pUnit, shooter.shot.hpSteps,
                                                            enemy.minRange, enemy.maxRange, actor, shooter.shot.distance);
                damage.withAmmo = damage.rawBase >= 0
                                      ? damage.rawBase
                                      : m_oracle.baseDamageWithAmmo(*shooter.pUnit, actor, shooter.shot.distance,
                                                                    enemy.minRange, enemy.maxRange);
                if (Coordinator::drawsCounter(shooter.direct, shooter.firingTile, property))
                {
                    damage.counterRawBase = m_oracle.baseDamageAgainst(actor, *shooter.pUnit, COUNTER_DISTANCE,
                                                                       unit.minRange, unit.maxRange);
                }
            }
            damage.weapon = Coordinator::shooterWeaponStatus(
                shooter.pUnit != nullptr, damage.rawBase, damage.withAmmo,
                Coordinator::repairPending(shooter.shot.hpSteps, shooter.onOwnBuilding));
            damage.resolution = Coordinator::pricedResolution(damage.weapon, shooter.onOwnBuilding,
                                                              shooter.firingTile != Coordinator::INVALID_TILE);
            damage.pricedHpSteps = Coordinator::pricedShooterHpSteps(shooter.shot.hpSteps, shooter.onOwnBuilding);
            damage.exactStatus = Coordinator::exactQueryStatus(damage.weapon, result.onProperty,
                                                               shooter.firingTile != Coordinator::INVALID_TILE);
            if (exactQuery == ExactQuery::Skipped && damage.exactStatus == ShotStatus::Computed)
            {
                damage.exactStatus = ShotStatus::NotQueried;
            }
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
            record.insert(KEY_TYPE_BASE_COST, static_cast<qint64>(shooter.cost));
            record.insert(KEY_PLANNER_TARGET_COST, shooter.plannerTargetCost
                                                       ? QJsonValue(static_cast<qint64>(*shooter.plannerTargetCost))
                                                       : QJsonValue());
            record.insert(KEY_CAPTURE_INTERRUPTION, QLatin1String(Record::interruptionName(shooter.interruption)));
            record.insert(Record::KEY_CAPTURE_COST, static_cast<qint64>(shooter.captureCost));
            record.insert(KEY_CAPTURE_STOCK_AGREES,
                          shooter.stockAgrees ? QJsonValue(*shooter.stockAgrees) : QJsonValue());
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

    void DangerAuditRecorder::addPricedWalks(QJsonObject & target, const std::vector<Shooter> & shooters,
                                             const ActorDamage & damage, const CaptureWalkInput & input,
                                             Coordinator::MilliFunds actorCost, const PropertyStockColumn & column) const
    {
        const std::vector<std::size_t> order = focusedOrderOf(damage);
        QJsonObject walks = target.value(KEY_WALKS).toObject();
        for (const DamageSource source : PRICED_SOURCES)
        {
            const CaptureWalkResult walk = runPriced(source, shooters, damage, order,
                                                     [&](const auto & priced, auto && damageAt, auto && counterCostAt)
            {
                return Coordinator::walkPriced(input, actorCost, priced, damageAt, counterCostAt);
            });
            walks.insert(pricedWalkName(source), Record::walkRecord(walk, &column, m_horizonTurns));
        }
        target.insert(KEY_WALKS, walks);
    }

    QJsonObject DangerAuditRecorder::replyRecord(const std::vector<Shooter> & shooters, const ActorDamage & damage,
                                                 double hp, Coordinator::MilliFunds actorCost,
                                                 const TilePoint & destination)
    {
        const std::vector<std::size_t> order = focusedOrderOf(damage);
        QJsonArray shooterFacts = shooterRecords(shooters);
        for (std::size_t index = 0; index < shooters.size(); ++index)
        {
            const ShotDamage & shot = damage.shots[index];
            QJsonObject record = shooterFacts.at(static_cast<qsizetype>(index)).toObject();
            record.insert(KEY_RESOLUTION, QLatin1String(Record::resolutionName(shot.resolution)));
            record.insert(KEY_PRICED_HP, shot.pricedHpSteps);
            record.insert(KEY_COUNTER_BASE, shot.counterRawBase < 0 ? QJsonValue() : QJsonValue(shot.counterRawBase));
            shooterFacts.replace(static_cast<qsizetype>(index), record);
        }
        QJsonObject reply{{KEY_SHOOTERS, shooterFacts}};
        addCapacity(reply, shooters, destination);
        for (const DamageSource source : PRICED_SOURCES)
        {
            const auto ordered = [&](std::int32_t hpSteps)
            {
                return inOrder(shotsAt(source, damage, hpSteps), order);
            };
            const std::optional<Coordinator::ReplyOutcome> largest =
                Coordinator::singleReply(hp, CaptureReplyRule::LargestShot, ordered);
            const std::optional<Coordinator::ReplyOutcome> focused =
                Coordinator::singleReply(hp, CaptureReplyRule::FocusedFire, ordered);
            const Coordinator::PricedReply priced = runPriced(source, shooters, damage, order,
                                                              [&](const auto & priceList, auto && damageAt,
                                                                  auto && counterCostAt)
            {
                return Coordinator::pricedReply(hp, actorCost, priceList, damageAt, counterCostAt);
            });
            QJsonArray pricedShots;
            for (std::size_t index = 0; index < shooters.size(); ++index)
            {
                const auto position = static_cast<std::size_t>(std::find(order.begin(), order.end(), index) - order.begin());
                pricedShots.append(Record::pricedShotRecord(priced.shots[position]));
            }
            QJsonObject pricedRecord = Record::outcomeRecord(priced.outcome);
            pricedRecord.insert(Record::KEY_PRICED_SHOTS, pricedShots);
            reply.insert(sourceName(source), QJsonObject{
                {KEY_RULE_R1, Record::outcomeRecord(largest)},
                {KEY_RULE_R2, Record::outcomeRecord(focused)},
                {KEY_RULE_P, pricedRecord},
            });
            if (source != LOAD_SOURCE)
            {
                continue;
            }
            for (std::size_t position = 0; position < order.size(); ++position)
            {
                ShooterLoad & load = m_shooterLoads[shooters[order[position]].shot.unitIndex];
                load.pricedFires += priced.outcome.fired[position] ? 1 : 0;
                if (!focused)
                {
                    ++load.focusedUnknown;
                }
                else if (focused->fired[position])
                {
                    ++load.focusedFires;
                }
            }
        }
        return reply;
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

    // An unmatched action takes the cost its other candidates carry; only an actor with none runs the cost scripts.
    Coordinator::MilliFunds DangerAuditRecorder::actorCostFor(const Coordinator::PlannedAction & action,
                                                              Unit & actor) const
    {
        for (const Coordinator::AssignmentActor & candidateActor : m_view.actors)
        {
            if (candidateActor.engineUnitId == action.unitId && !candidateActor.candidates.empty())
            {
                return candidateActor.candidates.front().actor.replacementCost;
            }
        }
        return Coordinator::toMilliFunds(actor.getUnitCosts());
    }

    QJsonObject DangerAuditRecorder::planRecord(const Coordinator::PlannedAction & action, qsizetype planSlot)
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
        Unit* pActor = liveUnit(actorIndex);
        if (pActor == nullptr)
        {
            record.insert(KEY_OMITTED, QLatin1String(OMITTED_NO_LIVE_UNIT));
            return record;
        }
        record.insert(KEY_HP_REAL, pActor->getHp());
        const Coordinator::CandidateBundle* pCandidate = candidateFor(action);
        const ActionFacts actionFacts = pCandidate == nullptr ? ActionFacts::Unmatched : ActionFacts::Candidate;
        const Coordinator::ActionHpOutcome outcome =
            pCandidate == nullptr ? Coordinator::ActionHpOutcome{.hpSteps = unit.hpRounded}
                                  : Coordinator::hpAfterAction(pCandidate->bundle, pCandidate->actor.hpSteps);
        const Coordinator::MilliFunds actorCost =
            pCandidate == nullptr ? actorCostFor(action, *pActor) : pCandidate->actor.replacementCost;
        const double hpAfter = Coordinator::realHpAfterAction(pActor->getHp(), outcome);
        record.insert(KEY_ACTION_FACTS, QLatin1String(actionFactsName(actionFacts)));
        record.insert(KEY_ACTOR_COST, static_cast<qint64>(actorCost));
        record.insert(KEY_HP_AFTER_ACTION, outcome.hpSteps);
        record.insert(KEY_HP_REAL_AFTER_ACTION, hpAfter);
        record.insert(KEY_HP_REAL_APPROXIMATE, outcome.realHpFromSteps);
        record.insert(KEY_COUNTER_APPLIED, outcome.counterApplied);
        record.insert(KEY_VICTIM, outcome.victimIndex == Coordinator::NO_UNIT ? QJsonValue() : QJsonValue(outcome.victimIndex));
        record.insert(KEY_VICTIM_HP, outcome.victimHpSteps);
        record.insert(KEY_DESTINATION_OWN_BUILDING, ownBuildingAt(action.destination, unit.ownerId));
        if (!Coordinator::replyStartKnown(pCandidate != nullptr, action.kind))
        {
            ++m_unmatchedFires;
            record.insert(KEY_REPLY, QJsonValue());
            return record;
        }
        const Coordinator::ActionHpOutcome planOutcome =
            pCandidate == nullptr ? outcome
                                  : Coordinator::hpAfterActionOn(pCandidate->bundle, pCandidate->actor.hpSteps, m_planChain);
        const double hpAfterPlan = Coordinator::realHpAfterAction(pActor->getHp(), planOutcome);
        record.insert(KEY_HP_AFTER_ACTION_PLAN, planOutcome.hpSteps);
        record.insert(KEY_HP_REAL_AFTER_ACTION_PLAN, hpAfterPlan);
        record.insert(KEY_HP_REAL_PLAN_APPROXIMATE, planOutcome.realHpFromSteps);
        record.insert(KEY_COUNTER_APPLIED_PLAN, planOutcome.counterApplied);
        record.insert(KEY_COUNTER_FROM_ISOLATED_VIEW, planOutcome.counterFromIsolatedView);
        // A unit our own action destroys in both views holds no post, so nothing replies to it.
        std::vector<Shooter> shooters;
        if (outcome.hpSteps > 0 || planOutcome.hpSteps > 0)
        {
            shooters = shootersAt(action.destination, outcome.victimIndex, outcome.victimHpSteps);
        }
        const ActorDamage damage = damageOn(shooters, actorIndex, *pActor, action.destination,
                                            captures ? ExactQuery::Engine : ExactQuery::Skipped);
        record.insert(KEY_REPLY, replyRecord(shooters, damage, hpAfter, actorCost, action.destination));
        m_replyEntries.push_back(ReplyEntry{
            .planSlot = planSlot,
            .unitId = action.unitId,
            .hp = hpAfterPlan,
            .cost = actorCost,
            .shooters = shooters,
            .damage = damage,
        });
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
        const CaptureWalkInput input = walkInput(*pActor, carried, candidateRate);
        addDanger(capture, damage, input, &column);
        addPricedWalks(capture, shooters, damage, input, actorCost, column);
        record.insert(KEY_CAPTURE, capture);
        return record;
    }

    // PlannedAction keeps no candidate index, so the candidate is found by the intent plannedActionFrom gives it.
    const Coordinator::CandidateBundle* DangerAuditRecorder::candidateFor(const Coordinator::PlannedAction & action) const
    {
        for (const Coordinator::AssignmentActor & actor : m_view.actors)
        {
            if (actor.engineUnitId != action.unitId)
            {
                continue;
            }
            for (const Coordinator::CandidateBundle & candidate : actor.candidates)
            {
                const Coordinator::PlannedAction intent =
                    Coordinator::plannedActionFrom(Coordinator::PlanActionIds{}, m_view.unitLinks, actor.engineUnitId,
                                                   candidate);
                if (intent.kind == action.kind && intent.path == action.path &&
                    intent.destination == action.destination && intent.target == action.target &&
                    intent.targetUnitId == action.targetUnitId)
                {
                    return &candidate;
                }
            }
        }
        return nullptr;
    }

    QJsonArray DangerAuditRecorder::shooterLoadRecords() const
    {
        QJsonArray records;
        for (const auto & [knowledgeIndex, load] : m_shooterLoads)
        {
            const KnownUnit & unit = known(knowledgeIndex);
            records.append(QJsonObject{
                {KEY_KNOWLEDGE, knowledgeIndex},
                {KEY_UNIT, Coordinator::linkOf(m_view.unitLinks, knowledgeIndex).engineUnitId},
                {KEY_TYPE, unit.unitId},
                {KEY_TILE, tileRecord(TilePoint{unit.x, unit.y})},
                {KEY_PRICED_FIRES, load.pricedFires},
                {KEY_FOCUSED_FIRES, load.focusedFires},
                {KEY_FOCUSED_UNKNOWN, load.focusedUnknown},
                {KEY_ONE_ACTION_UNIT, load.oneActionUnit ? QJsonValue(*load.oneActionUnit) : QJsonValue()},
                {KEY_ONE_ACTION_HP, load.oneActionHpSteps},
            });
        }
        return records;
    }

    void DangerAuditRecorder::addOneActionReplies(QJsonArray & plan)
    {
        const auto started = std::chrono::steady_clock::now();
        std::set<std::int32_t> pooled;
        std::vector<std::map<std::int32_t, std::size_t>> shooterSlots(m_replyEntries.size());
        std::vector<Coordinator::OneActionTarget> targets;
        targets.reserve(m_replyEntries.size());
        for (std::size_t entry = 0; entry < m_replyEntries.size(); ++entry)
        {
            const ReplyEntry & reply = m_replyEntries[entry];
            targets.push_back(Coordinator::OneActionTarget{reply.unitId, reply.hp, reply.cost});
            for (std::size_t slot = 0; slot < reply.shooters.size(); ++slot)
            {
                pooled.insert(reply.shooters[slot].shot.unitIndex);
                shooterSlots[entry].emplace(reply.shooters[slot].shot.unitIndex, slot);
            }
        }
        const std::vector<std::int32_t> pool(pooled.begin(), pooled.end());
        const Coordinator::TargetHpChain & planHp = m_planChain;
        for (const auto & [enemyIndex, hp] : planHp)
        {
            m_planKills += hp.after <= 0 ? 1 : 0;
            m_planDamaged += hp.after > 0 && hp.after < hp.before ? 1 : 0;
        }
        std::vector<std::int32_t> poolHp;
        poolHp.reserve(pool.size());
        for (const std::int32_t shooter : pool)
        {
            const auto damaged = planHp.find(shooter);
            poolHp.push_back(damaged == planHp.end() ? known(shooter).hpRounded : damaged->second.after);
            m_shooterLoads[shooter].oneActionHpSteps = poolHp.back();
        }
        const auto slotOf = [&](std::size_t shooter, std::size_t target) -> std::optional<std::size_t>
        {
            const auto found = shooterSlots[target].find(pool[shooter]);
            return found == shooterSlots[target].end() ? std::nullopt : std::optional<std::size_t>(found->second);
        };
        // The whole plan's damage, then the shooter's own building repair, sets the hp P1 prices it at.
        const auto shooterHpAt = [&](std::size_t shooter, std::size_t target)
        {
            return Coordinator::oneActionShooterHpSteps(poolHp[shooter],
                                                        m_replyEntries[target].shooters[*slotOf(shooter, target)].onOwnBuilding);
        };
        for (const DamageSource source : PRICED_SOURCES)
        {
            const Coordinator::OneActionReply reply = Coordinator::oneActionReply(
                targets, pool,
                [&](std::size_t shooter, std::size_t target)
                {
                    const std::optional<std::size_t> slot = slotOf(shooter, target);
                    return slot && shooterHpAt(shooter, target) > 0 &&
                           Coordinator::pricedShotPossible(m_replyEntries[target].damage.shots[*slot].resolution);
                },
                [&](std::size_t shooter, std::size_t target, std::int32_t hpSteps)
                {
                    const ReplyEntry & entry = m_replyEntries[target];
                    return pricedDamageAt(source, entry.damage, entry.damage.shots[*slotOf(shooter, target)],
                                          shooterHpAt(shooter, target), hpSteps);
                },
                [&](std::size_t shooter, std::size_t target, std::int32_t hpStepsAfter)
                {
                    const ReplyEntry & entry = m_replyEntries[target];
                    const std::size_t slot = *slotOf(shooter, target);
                    return Coordinator::counterCost(entry.damage.shots[slot].counterRawBase, hpStepsAfter,
                                                    shooterHpAt(shooter, target), entry.shooters[slot].cost);
                },
                [&](std::size_t shooter, std::size_t target)
                {
                    return m_replyEntries[target].shooters[*slotOf(shooter, target)].captureCost;
                });
            std::vector<QJsonArray> chosenBy(targets.size());
            for (const Coordinator::OneActionShot & shot : reply.shots)
            {
                chosenBy[shot.target].append(QJsonObject{
                    {KEY_KNOWLEDGE, pool[shot.shooter]},
                    {KEY_NET, static_cast<qint64>(shot.net)},
                });
            }
            for (std::size_t target = 0; target < targets.size(); ++target)
            {
                QJsonObject outcome = Record::outcomeRecord(reply.outcomes[target]);
                outcome.insert(KEY_SHOOTERS, chosenBy[target]);
                const qsizetype planSlot = m_replyEntries[target].planSlot;
                QJsonObject entry = plan.at(planSlot).toObject();
                QJsonObject replyObject = entry.value(KEY_REPLY).toObject();
                QJsonObject sourceRecord = replyObject.value(sourceName(source)).toObject();
                sourceRecord.insert(KEY_RULE_P1, outcome);
                replyObject.insert(sourceName(source), sourceRecord);
                entry.insert(KEY_REPLY, replyObject);
                plan.replace(planSlot, entry);
            }
            if (source != LOAD_SOURCE)
            {
                continue;
            }
            for (std::size_t shooter = 0; shooter < pool.size(); ++shooter)
            {
                if (reply.chosen[shooter] != Coordinator::NO_ONE_ACTION_TARGET)
                {
                    m_shooterLoads[pool[shooter]].oneActionUnit = targets[reply.chosen[shooter]].unitId;
                }
            }
        }
        m_oneActionNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                                started).count();
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
            plan.append(planRecord(m_view.plan.action(action), plan.size()));
        }
        addOneActionReplies(plan);
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
            {KEY_SHOOTER_LOADS, shooterLoadRecords()},
            {KEY_LOAD_SOURCE, QLatin1String(sourceName(LOAD_SOURCE))},
            {KEY_COUNTER_SOURCE, QLatin1String(SOURCE_BASE)},
            {KEY_REPAIR_STEPS, Coordinator::BUILDING_REPAIR_STEPS},
            {KEY_ONE_ACTION_NANOS, static_cast<qint64>(m_oneActionNanos)},
            {KEY_PER_UNIT_VIEW, QLatin1String(PER_UNIT_VIEW_ISOLATED)},
            {KEY_PLAN_KILLS, m_planKills},
            {KEY_PLAN_DAMAGED, m_planDamaged},
            {KEY_PLAN_UNMATCHED_FIRES, m_unmatchedFires},
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
