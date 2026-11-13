#include "ai/coordinator/capturedangeraudit.h"
#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
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
#include "ai/coordinator/capturedangerpricing.h"
#include "ai/coordinator/capturedangerrecord.h"
#include "ai/coordinator/capturedangerstream.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/replycheck.h"
#include "ai/coordinator/replyfacts.h"
#include "ai/coordinator/replyfactsbuilder.h"
#include "ai/coordinator/replyprediction.h"

#include "coreengine/settings.h"

#include "game/gamemap.h"
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
    using Coordinator::ReplyCheckLayer;
    using Coordinator::ShotReport;
    using Coordinator::ShotStatus;
    using Coordinator::TerrainDefenseRule;
    using Coordinator::TilePoint;

    constexpr std::int32_t DANGER_AUDIT_SCHEMA = 11;
    constexpr auto DANGER_AUDIT_FILE = "coordinated-ai-danger.jsonl";

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
    constexpr auto KEY_REPLY_PRICING = "replyPricing";
    constexpr auto KEY_SOURCE = "source";
    constexpr auto KEY_NEXT_SHOT_SCALED = "nextShotScaled";
    constexpr auto KEY_INSTALLED = "installed";
    constexpr auto KEY_CAPITAL = "capital";
    constexpr auto KEY_NEXT_SHOT = "nextShot";
    constexpr auto KEY_TOTAL = "total";
    constexpr auto KEY_EXPOSURE_CHARGED = "exposureCharged";
    constexpr auto KEY_UNITS_SHOT = "unitsShot";
    constexpr auto KEY_UNITS_DESTROYED = "unitsDestroyed";
    constexpr auto KEY_UNITS = "units";
    constexpr auto KEY_SLOT = "slot";
    constexpr auto KEY_REASON = "reason";
    constexpr auto KEY_HP_BEFORE = "hpBefore";
    constexpr auto KEY_LOSS = "loss";
    constexpr auto KEY_CANDIDATES = "candidates";
    constexpr auto KEY_PRICED = "priced";
    constexpr auto KEY_EXACT = "exact";
    constexpr auto KEY_OPEN = "open";
    constexpr auto KEY_COUNT = "count";
    constexpr auto KEY_POSITIVE = "positive";
    constexpr auto KEY_BEST = "best";
    constexpr auto KEY_BEST_EXACT = "bestExact";
    constexpr auto KEY_BEST_OPEN = "bestOpen";
    constexpr auto KEY_CHANGES_PRICED = "changesPriced";
    constexpr auto KEY_PAIRS_EXAMINED = "pairsExamined";
    constexpr auto KEY_UNMATCHED_ENTRIES = "unmatchedEntries";
    constexpr auto KEY_CANDIDATE = "candidate";
    constexpr auto KEY_TARGET = "target";
    constexpr auto KEY_STOCK_KEY_UNCHANGED = "stockKeyUnchanged";
    constexpr auto KEY_CHAIN_INTERACTION = "chainInteraction";
    constexpr auto KEY_FIRING_TILES_SHARED = "firingTilesShared";
    constexpr auto KEY_PLANNER_DELTA = "plannerDelta";
    constexpr auto KEY_SEATED_DELTA = "seatedDelta";
    constexpr auto KEY_REPLY_DELTA = "replyDelta";
    constexpr auto KEY_GAIN = "gain";
    constexpr auto KEY_SERVICE_INTERACTION = "serviceInteraction";
    constexpr auto KEY_TARGET_CONTINUATION_DELTA = "targetContinuationDelta";
    constexpr auto KEY_TARGET_CONTINUATION_CHARGED = "targetContinuationCharged";
    constexpr auto KEY_BASELINE_CONSISTENT = "baselineConsistent";
    constexpr auto KEY_INSTALLED_PAIRS_EXAMINED = "installedPairsExamined";
    constexpr auto KEY_INVALID = "invalid";
    constexpr auto KEY_HAS_STOCK_ROW = "hasStockRow";
    constexpr auto REASON_SHOT = "SHOT";
    constexpr auto REASON_HITS_SHOOTER = "HITS_SHOOTER";
    constexpr auto OMITTED_DESTINATION_TAKEN = "DESTINATION_TAKEN";
    constexpr auto OMITTED_TARGET_GONE = "TARGET_GONE";
    constexpr auto OMITTED_BREAKS_LATER_ACTION = "BREAKS_LATER_ACTION";
    constexpr auto KEY_STOCK_REPLY_QUOTE = "stockReplyQuote";
    constexpr auto KEY_TODAY = "today";
    constexpr auto KEY_AFTER_OWN_ACTIONS = "afterOwnActions";
    constexpr auto KEY_AFTER_REPLY = "afterReply";
    constexpr auto KEY_COUNTER_LOSS = "counterLoss";
    constexpr auto KEY_REPLY_LOSS = "replyLoss";
    constexpr auto KEY_COUNTER_STEPS = "counterSteps";
    constexpr auto KEY_REPLY_STEPS = "replySteps";
    constexpr auto KEY_DEATH_CAUSE = "deathCause";
    constexpr auto KEY_COMPLETED_CAPTURE = "completedCapture";
    constexpr auto KEY_DIED_ATTACKING = "diedAttacking";
    constexpr auto KEY_KILLED_BY_REPLY = "killedByReply";
    constexpr auto KEY_DAMAGED_ALIVE = "damagedAlive";
    constexpr auto KEY_ROW = "row";
    constexpr auto KEY_KEY = "key";
    constexpr auto KEY_OURS = "ours";
    constexpr auto KEY_OWNED = "owned";
    constexpr auto KEY_VALID = "valid";
    constexpr auto KEY_WITNESS_REPLAYS = "witnessReplays";
    constexpr auto KEY_AVAILABLE = "available";
    constexpr auto KEY_ACTIONS = "actions";
    constexpr auto KEY_CLASSES_BUILT = "classesBuilt";
    constexpr auto KEY_OWN_ACTIONS = "ownActions";
    constexpr auto KEY_OWN_ACTION_KNOWN = "ownActionKnown";
    constexpr auto KEY_ISOLATED_COUNTER_STEPS = "isolatedCounterSteps";
    constexpr auto KEY_COUNTER_STEPS_USED = "counterStepsUsed";
    constexpr auto KEY_COUNTER_SCALED = "counterScaled";
    constexpr auto KEY_CERTAIN_COUNTER_UNITS = "certainCounterUnits";
    constexpr auto KEY_SCALED_COUNTER_UNITS = "scaledCounterUnits";
    constexpr auto KEY_SCALED_COUNTER_SURVIVAL_DIFFERENCES = "scaledCounterSurvivalDifferences";
    constexpr auto KEY_OWN_HP_DIFFERENCES = "ownHpDifferences";
    constexpr auto KEY_REPLY_OUTCOME_DIFFERENCES = "replyOutcomeDifferences";
    constexpr auto KEY_OWN_HP_COMPARISONS = "ownHpComparisons";
    constexpr auto KEY_REPLY_OUTCOME_COMPARISONS = "replyOutcomeComparisons";
    constexpr auto KEY_PREPARATION_NANOS = "preparationNanos";
    constexpr auto KEY_PRICING_NANOS = "pricingNanos";
    constexpr auto KEY_REPLAY_NANOS = "replayNanos";
    constexpr auto KEY_CHAIN_NANOS = "chainNanos";
    constexpr auto KEY_PLAN_REPLAY = "planReplay";
    constexpr auto KEY_PRESENT = "present";
    constexpr auto KEY_COMPLETE = "complete";
    constexpr auto KEY_ENTRIES = "entries";
    constexpr auto KEY_AGREES = "agrees";
    constexpr auto KEY_HP_STEPS_BEFORE = "hpStepsBefore";
    constexpr auto KEY_STEPS_LOST = "stepsLost";
    constexpr auto KEY_LAYER = "layer";
    constexpr auto KEY_AUDIT_ENTRIES = "auditEntries";
    constexpr auto KEY_AUDIT_ONLY = "auditOnly";
    constexpr auto KEY_AUDIT_INPUTS_AGREE = "auditInputsAgree";
    constexpr auto KEY_AUDIT_INPUTS_LAYER = "auditInputsLayer";
    constexpr auto KEY_TELEMETRY = "telemetry";
    constexpr auto KEY_OCCUPANCY_AGREES = "occupancyAgrees";
    constexpr auto KEY_AUDIT_HP_BEFORE = "auditHpBefore";
    constexpr auto KEY_ENEMY_HP = "enemyHp";
    constexpr auto KEY_PLANNER_HP = "plannerHp";
    constexpr auto KEY_AUDIT_HP = "auditHp";
    constexpr auto LAYER_ENTRY = "ENTRY";
    constexpr auto LAYER_START_HP = "START_HP";
    constexpr auto LAYER_ENEMY_HP = "ENEMY_HP";
    constexpr auto LAYER_FACTS = "FACTS";
    constexpr auto LAYER_OUTCOME = "OUTCOME";

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

    QJsonValue layerValue(ReplyCheckLayer layer)
    {
        switch (layer)
        {
            case ReplyCheckLayer::Agrees:
                break;
            case ReplyCheckLayer::Entry:
                return QLatin1String(LAYER_ENTRY);
            case ReplyCheckLayer::StartHp:
                return QLatin1String(LAYER_START_HP);
            case ReplyCheckLayer::EnemyHp:
                return QLatin1String(LAYER_ENEMY_HP);
            case ReplyCheckLayer::Facts:
                return QLatin1String(LAYER_FACTS);
            case ReplyCheckLayer::Outcome:
                return QLatin1String(LAYER_OUTCOME);
        }
        return QJsonValue();
    }

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

    const char* reasonName(Coordinator::ChangeReason reason)
    {
        return reason == Coordinator::ChangeReason::Shot ? REASON_SHOT : REASON_HITS_SHOOTER;
    }

    constexpr std::array OMISSIONS{
        Coordinator::ChangeOmission::DestinationTaken,
        Coordinator::ChangeOmission::TargetGone,
        Coordinator::ChangeOmission::BreaksLaterAction,
    };

    const char* omissionName(Coordinator::ChangeOmission omission)
    {
        switch (omission)
        {
            case Coordinator::ChangeOmission::DestinationTaken:
                return OMITTED_DESTINATION_TAKEN;
            case Coordinator::ChangeOmission::TargetGone:
                return OMITTED_TARGET_GONE;
            case Coordinator::ChangeOmission::None:
            case Coordinator::ChangeOmission::BreaksLaterAction:
                break;
        }
        return OMITTED_BREAKS_LATER_ACTION;
    }

    QJsonObject lossRecord(const Coordinator::ReplyLoss & loss)
    {
        return QJsonObject{
            {KEY_CAPITAL, static_cast<qint64>(loss.capital)},
            {KEY_NEXT_SHOT, static_cast<qint64>(loss.nextShot)},
            {KEY_CAPTURE, static_cast<qint64>(loss.capture)},
        };
    }

    QJsonObject lossTotalRecord(const Coordinator::ReplyLoss & loss)
    {
        QJsonObject record = lossRecord(loss);
        record.insert(KEY_TOTAL, static_cast<qint64>(loss.total()));
        return record;
    }

    using ChangeBucket = Coordinator::ChangeBucket<QJsonObject>;

    QJsonValue bestRecord(const ChangeBucket & bucket)
    {
        return bucket.bestGain ? QJsonValue(bucket.best) : QJsonValue();
    }

    QJsonObject bucketRecord(const ChangeBucket & bucket)
    {
        return QJsonObject{{KEY_COUNT, bucket.count}, {KEY_POSITIVE, bucket.positive}, {KEY_BEST, bestRecord(bucket)}};
    }

    // A null pUnit is a reaching enemy whose live unit was not found; its shot stays unknown.
    struct Shooter
    {
        Coordinator::EnemyShot shot;
        Unit* pUnit{nullptr};
        TilePoint firingTile{Coordinator::INVALID_TILE};
        std::optional<Coordinator::MilliFunds> plannerTargetCost;
        // Indexes the tile facts' shooters and the actor facts' shots alike.
        std::size_t factsSlot{0};
        const Coordinator::ReplyShooterFacts* pFacts{nullptr};
    };

    // The shared reply facts plus the engine's own report; base and luck use the shooter's rounded hp.
    struct ShotDamage : Coordinator::ResolvedReplyShot
    {
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
        std::int32_t knowledgeIndex{Coordinator::NO_UNIT};
        Unit* pActor{nullptr};
        const Coordinator::CandidateBundle* pCandidate{nullptr};
        std::int32_t actorHpSteps{0};
        TilePoint destination{Coordinator::INVALID_TILE};
        std::optional<Coordinator::CaptureStake> capture;
        // The audit lists shooters while either the isolated or the plan view leaves the actor alive.
        bool listsShooters{false};
    };

    struct CaptureSite
    {
        std::int32_t carried{0};
        std::int32_t rate{0};
        PropertyStockColumn column;
    };

    // Fire targets and serviced partners are knowledge indices; a destination is held only for a live slot.
    struct InstalledReply
    {
        std::vector<std::set<std::int32_t>> slotTargets;
        std::vector<std::set<std::int32_t>> slotPartners;
        std::vector<std::optional<TilePoint>> slotDestinations;
        std::vector<Coordinator::PlanChainEntry> chainEntries;
        Coordinator::PlanChainRun chain;
        std::vector<ReplyEntry> entries;
        Coordinator::PlanReplyRun run;
        std::vector<Coordinator::ReplyLoss> losses;
        Coordinator::ReplyLoss total;
    };

    struct PricedChange
    {
        Coordinator::ChangeOmission omission{Coordinator::ChangeOmission::None};
        bool exact{false};
        Coordinator::MilliFunds gain{0};
        std::int64_t pairsExamined{0};
        QJsonObject record;
    };

    // Units both models list, matched by engine unit id: auditOf per planner entry, plannerOf per audit entry.
    struct ReplyPairing
    {
        std::vector<std::optional<std::size_t>> auditOf;
        std::vector<std::optional<std::size_t>> plannerOf;
    };

    // A first-run layer with what explains the hp layers; the hp fields are filled only for their own layer.
    struct ReplyCheck
    {
        ReplyCheckLayer layer{ReplyCheckLayer::Agrees};
        double auditHpBefore{0};
        std::vector<Coordinator::ReplyHpGap> enemyHp;
    };

    QJsonArray enemyHpRecord(std::span<const Coordinator::ReplyHpGap> gaps)
    {
        QJsonArray records;
        for (const Coordinator::ReplyHpGap & gap : gaps)
        {
            records.append(QJsonObject{
                {KEY_KNOWLEDGE, gap.knowledge},
                {KEY_PLANNER_HP, gap.plannerHp},
                {KEY_AUDIT_HP, gap.auditHp},
            });
        }
        return records;
    }

    QJsonObject replyTelemetryRecord(const Coordinator::ReplyPredictionTelemetry & telemetry)
    {
        QJsonObject record;
        for (const Coordinator::ReplyTelemetryField & field : Coordinator::replyTelemetryFields(telemetry))
        {
            record.insert(QLatin1String(field.name), static_cast<qint64>(field.value));
        }
        return record;
    }

    Coordinator::ReplyCheckFacts auditCheckFacts(const ReplyEntry & entry)
    {
        Coordinator::ReplyCheckFacts facts{.terrain = entry.damage.terrain};
        facts.shots.reserve(entry.shooters.size());
        for (std::size_t index = 0; index < entry.shooters.size(); ++index)
        {
            const Shooter & shooter = entry.shooters[index];
            facts.shots.push_back(Coordinator::replyCheckShot(*shooter.pFacts, shooter.firingTile,
                                                              entry.damage.shots[index]));
        }
        return facts;
    }

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
        if (source == DamageSource::Base)
        {
            const double rawBase = Coordinator::pricedRawBase(damage.resolution, damage.rawBase, damage.withAmmo);
            return Coordinator::hpScaledBase(rawBase, shooterHpSteps);
        }
        return Coordinator::pricedTerrainDamage(actor.terrain, damage, shooterHpSteps, defenderHpSteps);
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
            priced.push_back(
                Coordinator::PricedShooter{damage.shots[index].resolution, shooters[index].pFacts->captureCost});
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
                                                       shooters[order[position]].pFacts->cost);
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

    enum class PlanOccupancyView : std::int8_t
    {
        Legacy,
        Live,
    };

    class DangerAuditRecorder
    {
    public:
        explicit DangerAuditRecorder(const Coordinator::CaptureDangerAuditView & view)
            : m_view(view),
              m_oracle(view.map),
              m_facts(view.map, view.knowledge, view.enemyReach, view.properties, view.propertyStock, m_oracle),
              m_horizonTurns(view.propertyStock.horizonTurns()),
              m_standing(standingOccupants()),
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

        std::vector<Coordinator::OccupantAt> standingOccupants();
        Coordinator::PlannedOccupancy plannedOccupancy(std::int32_t movedUnitId = Coordinator::NO_UNIT,
                                                       const TilePoint & movedTo = Coordinator::INVALID_TILE,
                                                       PlanOccupancyView scope = PlanOccupancyView::Legacy) const;
        Unit* liveUnit(std::int32_t knowledgeIndex);
        FiringCapacity firingCapacity(const std::vector<Shooter> & shooters) const;
        std::vector<Shooter> shootersAt(const TilePoint & property, std::int32_t victimIndex = Coordinator::NO_UNIT,
                                        std::int32_t victimHpSteps = 0);
        std::vector<Shooter> shootersAmong(const Coordinator::PlannedOccupancy & occupancy, const TilePoint & property,
                                           std::int32_t victimIndex, std::int32_t victimHpSteps);
        ActorDamage damageOn(const std::vector<Shooter> & shooters, std::int32_t actorIndex, Unit & actor,
                             const TilePoint & property, ExactQuery exactQuery = ExactQuery::Engine);
        CaptureWalkInput walkInput(Unit & actor, std::int32_t carriedPoints, std::int32_t rate) const;
        QJsonObject standingRecord(const KnownUnit & shooter, std::int32_t shooterIndex) const;
        QJsonArray shooterRecords(const std::vector<Shooter> & shooters) const;
        void addCapacity(QJsonObject & target, const std::vector<Shooter> & shooters) const;
        void addDanger(QJsonObject & target, const ActorDamage & damage, const CaptureWalkInput & input,
                       const PropertyStockColumn* pColumn) const;
        void addPricedWalks(QJsonObject & target, const std::vector<Shooter> & shooters, const ActorDamage & damage,
                            const CaptureWalkInput & input, Coordinator::MilliFunds actorCost,
                            const PropertyStockColumn & column) const;
        QJsonObject replyRecord(const std::vector<Shooter> & shooters, const ActorDamage & damage, double hp,
                                Coordinator::MilliFunds actorCost);
        QJsonObject nodeRecord(std::int32_t propertyIndex, const std::vector<Shooter> & shooters,
                               const std::vector<CaptureActor> & actors);
        const Coordinator::CandidateBundle* candidateFor(const Coordinator::PlannedAction & action) const;
        Coordinator::MilliFunds actorCostFor(const Coordinator::PlannedAction & action, Unit & actor) const;
        CaptureSite captureSite(std::int32_t propertyIndex, std::int32_t actorIndex, Unit & actor,
                                const TilePoint & destination) const;
        QJsonObject planRecord(const Coordinator::PlannedAction & action, qsizetype planSlot);
        Coordinator::PlanReplyRun oneActionRun(std::span<const ReplyEntry> entries,
                                               const Coordinator::TargetHpChain & chain, DamageSource source) const;
        void addOneActionReplies(QJsonArray & plan);
        QJsonArray shooterLoadRecords() const;
        std::int32_t knowledgeIndexOf(std::int32_t engineUnitId) const;
        const Coordinator::AssignmentActor* actorOf(std::int32_t engineUnitId) const;
        std::vector<Coordinator::ReplyLoss> replyLosses(std::span<const ReplyEntry> entries,
                                                        const Coordinator::OneActionReply & reply) const;
        void addSlotFacts(InstalledReply & installed) const;
        bool firingTilesMove(std::span<const ReplyEntry> entries, std::size_t changed,
                             const Coordinator::PlannedOccupancy & occupancy) const;
        bool chainInteraction(const InstalledReply & installed, qsizetype planSlot,
                              const Coordinator::CandidateBundle & installedCandidate,
                              const Coordinator::CandidateBundle & replacement) const;
        bool entersStockKey(const Coordinator::PlannedAction & action, std::int32_t knowledgeIndex) const;
        bool stockKeyUnchangedFor(const Coordinator::PlannedAction & action, std::int32_t knowledgeIndex,
                                  const Coordinator::CandidateBundle & replacement) const;
        ReplyEntry changedEntry(const ReplyEntry & installed, const Coordinator::CandidateBundle & replacement,
                                const Coordinator::ActionHpOutcome & planOutcome,
                                const Coordinator::PlannedOccupancy & occupancy);
        std::vector<ReplyEntry> entriesOn(const Coordinator::PlanChainRun & chain) const;
        InstalledReply installedReply() const;
        bool baselineConsistent(const InstalledReply & installed) const;
        PricedChange priceChange(const InstalledReply & installed, std::size_t entryIndex,
                                 Coordinator::ChangeReason reason, std::size_t candidateIndex,
                                 const Coordinator::CandidateBundle & replacement);
        QJsonObject replyPricingRecord();
        std::int32_t chainHp(const Coordinator::TargetHpChain & chain, std::int32_t shooterIndex) const;
        std::map<std::int32_t, std::int32_t> poolHp(const std::set<std::int32_t> & shooters,
                                                    const Coordinator::TargetHpChain & chain) const;
        std::vector<Coordinator::PlanReplyAction> planReplyActions(const Coordinator::OrderingResult & ordering) const;
        Coordinator::ReplyBoard replyBoard() const;
        ReplyPairing pairEntries(const Coordinator::ResolvedPlanReply & resolved) const;
        std::vector<ReplyCheck> firstRunChecks(const Coordinator::ResolvedPlanReply & resolved,
                                               const Coordinator::PlanReplyRun & run,
                                               const ReplyPairing & pairing) const;
        std::vector<std::optional<ReplyCheckLayer>> secondRunLayers(
            std::span<const Coordinator::PlanReplyAction> actions, const ReplyPairing & pairing) const;
        QJsonObject planReplayRecord() const;
        Record::StockPlanMeasurement stockPlanMeasurement();
        void prepareStockReply(const Record::StockPlanMeasurement & measurement,
                               std::vector<Coordinator::PlanReplyEntry> & replies,
                               std::span<Coordinator::ReplyTargetFacts> targets);
        QJsonObject stockReplyQuoteRecord(const QJsonArray & plan);

        const Coordinator::CaptureDangerAuditView & m_view;
        Coordinator::DamageOracle m_oracle;
        // Declared after m_oracle, which it borrows.
        Coordinator::ReplyFactsBuilder m_facts;
        std::int32_t m_horizonTurns{0};
        // Declared before m_standing, whose initializer counts into it.
        std::int32_t m_alliedOccupants{0};
        std::vector<Coordinator::OccupantAt> m_standing;
        Coordinator::PlannedOccupancy m_occupancy;
        std::set<std::int32_t> m_missingLoaded;
        std::set<std::int32_t> m_missingAbsent;
        std::map<std::int32_t, Coordinator::MilliFunds> m_plannerCosts;
        std::map<std::int32_t, ShooterLoad> m_shooterLoads;
        std::vector<ReplyEntry> m_replyEntries;
        // The plan view: one target chain filled in plan order.
        Coordinator::TargetHpChain m_planChain;
        std::vector<std::int32_t> m_recordedPool;
        std::vector<std::size_t> m_recordedChosen;
        std::vector<Coordinator::ReplyOutcome> m_recordedOutcomes;
        std::int32_t m_unmatchedFires{0};
        std::int32_t m_planKills{0};
        std::int32_t m_planDamaged{0};
        std::int64_t m_oneActionNanos{0};
    };

    std::vector<Coordinator::OccupantAt> DangerAuditRecorder::standingOccupants()
    {
        Coordinator::ReplyBoard board = Coordinator::replyBoardOf(m_view.knowledge);
        m_alliedOccupants = board.alliedOccupants;
        return std::move(board.standing);
    }

    // movedUnitId ends on movedTo instead of its planned destination.
    Coordinator::PlannedOccupancy DangerAuditRecorder::plannedOccupancy(std::int32_t movedUnitId,
                                                                        const TilePoint & movedTo, PlanOccupancyView scope) const
    {
        std::vector<Coordinator::PlannedMove> moves;
        for (std::int32_t action = 0; action < m_view.plan.actionCount(); ++action)
        {
            const Coordinator::PlannedAction & planned = m_view.plan.action(action);
            if (scope == PlanOccupancyView::Live && !Coordinator::isLiveState(planned.state))
            {
                continue;
            }
            const std::int32_t mover = knowledgeIndexOf(planned.unitId);
            if (mover != Coordinator::NO_UNIT)
            {
                const KnownUnit & unit = known(mover);
                const TilePoint destination = planned.unitId == movedUnitId ? movedTo : planned.destination;
                moves.push_back(Coordinator::PlannedMove{mover, TilePoint{unit.x, unit.y}, destination});
            }
        }
        return Coordinator::PlannedOccupancy(m_view.knowledge.width(), m_view.knowledge.height(), m_standing, moves);
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

    // Direct shooters each pick a tile alone, so more of them than free tiles makes R2 an upper bound.
    FiringCapacity DangerAuditRecorder::firingCapacity(const std::vector<Shooter> & shooters) const
    {
        FiringCapacity capacity;
        std::vector<TilePoint> tiles;
        for (const Shooter & shooter : shooters)
        {
            if (!shooter.pFacts->direct)
            {
                continue;
            }
            ++capacity.directShooters;
            for (const TilePoint & tile : shooter.pFacts->firingTiles)
            {
                if (m_occupancy.at(tile.x, tile.y) == Coordinator::NO_UNIT)
                {
                    tiles.push_back(tile);
                }
            }
        }
        capacity.freeTiles = Coordinator::distinctTileCount(std::move(tiles));
        return capacity;
    }

    // A victim our shot leaves alive replies at its hp after the shot; a killed one does not reply.
    std::vector<Shooter> DangerAuditRecorder::shootersAt(const TilePoint & property, std::int32_t victimIndex,
                                                         std::int32_t victimHpSteps)
    {
        return shootersAmong(m_occupancy, property, victimIndex, victimHpSteps);
    }

    std::vector<Shooter> DangerAuditRecorder::shootersAmong(const Coordinator::PlannedOccupancy & occupancy,
                                                            const TilePoint & property, std::int32_t victimIndex,
                                                            std::int32_t victimHpSteps)
    {
        std::vector<Shooter> shooters;
        const std::shared_ptr<const Coordinator::ReplyTileFacts> tile = m_facts.tileFacts(property);
        for (std::size_t slot = 0; slot < tile->shooters.size(); ++slot)
        {
            const Coordinator::ReplyShooterFacts & facts = tile->shooters[slot];
            const std::int32_t hpSteps = Coordinator::replyShooterHpSteps(facts, victimIndex, victimHpSteps);
            if (hpSteps <= 0)
            {
                continue;
            }
            std::optional<Coordinator::MilliFunds> plannerTargetCost;
            const auto planned = m_plannerCosts.find(facts.shooterIndex);
            if (planned != m_plannerCosts.end())
            {
                plannerTargetCost = planned->second;
            }
            shooters.push_back(Shooter{
                .shot = Coordinator::EnemyShot{.slot = facts.slot, .unitIndex = facts.shooterIndex, .tile = facts.tile,
                                               .hpSteps = hpSteps, .distance = facts.distance},
                .pUnit = liveUnit(facts.shooterIndex),
                .firingTile = Coordinator::replyFiringTile(facts, occupancy),
                .plannerTargetCost = plannerTargetCost,
                .factsSlot = slot,
                .pFacts = &facts,
            });
        }
        return shooters;
    }

    // Every shooter comes from shootersAmong on this same property, so its facts share the actor facts' tile.
    ActorDamage DangerAuditRecorder::damageOn(const std::vector<Shooter> & shooters, std::int32_t actorIndex,
                                              Unit & actor, const TilePoint & property, ExactQuery exactQuery)
    {
        const KnownUnit & unit = known(actorIndex);
        const std::shared_ptr<const Coordinator::ReplyFacts> facts = m_facts.factsFor(actorIndex, actor, property);
        ActorDamage result;
        result.onProperty = TilePoint{unit.x, unit.y} == property;
        result.terrain = facts->terrain;
        result.shots.reserve(shooters.size());
        for (const Shooter & shooter : shooters)
        {
            ShotDamage damage;
            static_cast<Coordinator::ResolvedReplyShot &>(damage) = Coordinator::resolveReplyShot(
                *shooter.pFacts, facts->shots[shooter.factsSlot], shooter.shot.hpSteps, shooter.firingTile, property);
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
            const Coordinator::ReplyShooterFacts & facts = *shooter.pFacts;
            record.insert(KEY_DIRECT, facts.direct);
            record.insert(KEY_ON_OWN_BUILDING, facts.onOwnBuilding);
            record.insert(KEY_TILE, tileRecord(shooter.shot.tile));
            record.insert(KEY_DISTANCE, shooter.shot.distance);
            record.insert(KEY_MUST_MOVE, facts.mustMove);
            record.insert(KEY_FIRING_TILE, shooter.firingTile == Coordinator::INVALID_TILE
                                               ? QJsonValue()
                                               : QJsonValue(tileRecord(shooter.firingTile)));
            record.insert(KEY_TYPE_BASE_COST, static_cast<qint64>(facts.cost));
            record.insert(KEY_PLANNER_TARGET_COST, shooter.plannerTargetCost
                                                       ? QJsonValue(static_cast<qint64>(*shooter.plannerTargetCost))
                                                       : QJsonValue());
            record.insert(KEY_CAPTURE_INTERRUPTION, QLatin1String(Record::interruptionName(facts.interruption)));
            record.insert(Record::KEY_CAPTURE_COST, static_cast<qint64>(facts.captureCost));
            record.insert(KEY_CAPTURE_STOCK_AGREES, facts.stockAgrees ? QJsonValue(*facts.stockAgrees) : QJsonValue());
            records.append(record);
        }
        return records;
    }

    void DangerAuditRecorder::addCapacity(QJsonObject & target, const std::vector<Shooter> & shooters) const
    {
        const FiringCapacity capacity = firingCapacity(shooters);
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
                                                 double hp, Coordinator::MilliFunds actorCost)
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
        addCapacity(reply, shooters);
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
        addCapacity(node, shooters);
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
        const bool captures = Coordinator::capturesInPlan(action.kind);
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
        record.insert(KEY_DESTINATION_OWN_BUILDING,
                      Coordinator::ownBuildingAt(m_view.map, action.destination, unit.ownerId));
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
        const bool listsShooters = outcome.hpSteps > 0 || planOutcome.hpSteps > 0;
        if (listsShooters)
        {
            shooters = shootersAt(action.destination, outcome.victimIndex, outcome.victimHpSteps);
        }
        const ActorDamage damage = damageOn(shooters, actorIndex, *pActor, action.destination,
                                            captures ? ExactQuery::Engine : ExactQuery::Skipped);
        record.insert(KEY_REPLY, replyRecord(shooters, damage, hpAfter, actorCost));
        m_replyEntries.push_back(ReplyEntry{
            .planSlot = planSlot,
            .unitId = action.unitId,
            .hp = hpAfterPlan,
            .cost = actorCost,
            .shooters = shooters,
            .damage = damage,
            .knowledgeIndex = actorIndex,
            .pActor = pActor,
            .pCandidate = pCandidate,
            .actorHpSteps = pCandidate == nullptr ? unit.hpRounded : pCandidate->actor.hpSteps,
            .destination = action.destination,
            .listsShooters = listsShooters,
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
        const CaptureSite site = captureSite(propertyIndex, actorIndex, *pActor, action.destination);
        const std::int32_t carried = site.carried;
        const std::int32_t candidateRate = site.rate;
        record.insert(KEY_CANDIDATE_CAPTURE_RATE, candidateRate);
        const PropertyStockColumn & column = site.column;
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
        addCapacity(capture, shooters);
        const CaptureWalkInput input = walkInput(*pActor, carried, candidateRate);
        addDanger(capture, damage, input, &column);
        addPricedWalks(capture, shooters, damage, input, actorCost, column);
        record.insert(KEY_CAPTURE, capture);
        m_replyEntries.back().capture = Coordinator::CaptureStake{.column = column, .input = input};
        return record;
    }

    // The danger walks use the candidate's destination rate; the stock model assumes the origin rate.
    CaptureSite DangerAuditRecorder::captureSite(std::int32_t propertyIndex, std::int32_t actorIndex, Unit & actor,
                                                 const TilePoint & destination) const
    {
        const Coordinator::PropertyFacts & facts = m_view.properties[static_cast<std::size_t>(propertyIndex)];
        return CaptureSite{
            .carried = Coordinator::carriedCapturePoints(facts.capturerIndex, facts.capturePoints, actorIndex),
            .rate = actor.getCaptureRate(QPoint(destination.x, destination.y)),
            .column = PropertyStockColumn{
                .tile = destination,
                .income = Coordinator::captureIncome(facts),
                .ownerBefore = ownerOf(facts),
            },
        };
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

    Coordinator::PlanReplyRun DangerAuditRecorder::oneActionRun(std::span<const ReplyEntry> entries,
                                                                const Coordinator::TargetHpChain & chain,
                                                                DamageSource source) const
    {
        std::vector<Coordinator::ReplyTargetFacts> targets;
        targets.reserve(entries.size());
        for (const ReplyEntry & entry : entries)
        {
            Coordinator::ReplyTargetFacts facts{
                .target = Coordinator::OneActionTarget{entry.unitId, entry.hp, entry.cost},
            };
            facts.shooters.reserve(entry.shooters.size());
            for (std::size_t slot = 0; slot < entry.shooters.size(); ++slot)
            {
                facts.shooters.push_back(
                    Coordinator::replySlotFacts(*entry.shooters[slot].pFacts, entry.damage.shots[slot]));
            }
            targets.push_back(std::move(facts));
        }
        return Coordinator::planReply(
            targets, chain, [&](std::int32_t shooter) { return known(shooter).hpRounded; },
            [&](std::size_t target, std::size_t slot, std::int32_t shooterHpSteps, std::int32_t hpSteps)
            {
                const ReplyEntry & entry = entries[target];
                return pricedDamageAt(source, entry.damage, entry.damage.shots[slot], shooterHpSteps, hpSteps);
            });
    }

    void DangerAuditRecorder::addOneActionReplies(QJsonArray & plan)
    {
        const auto started = std::chrono::steady_clock::now();
        for (const auto & [enemyIndex, hp] : m_planChain)
        {
            m_planKills += hp.after <= 0 ? 1 : 0;
            m_planDamaged += hp.after > 0 && hp.after < hp.before ? 1 : 0;
        }
        for (const DamageSource source : PRICED_SOURCES)
        {
            const Coordinator::PlanReplyRun run = oneActionRun(m_replyEntries, m_planChain, source);
            const std::vector<std::int32_t> & pool = run.pool;
            const Coordinator::OneActionReply & reply = run.reply;
            const std::size_t targetCount = m_replyEntries.size();
            std::vector<QJsonArray> chosenBy(targetCount);
            for (const Coordinator::OneActionShot & shot : reply.shots)
            {
                chosenBy[shot.target].append(QJsonObject{
                    {KEY_KNOWLEDGE, pool[shot.shooter]},
                    {KEY_NET, static_cast<qint64>(shot.net)},
                });
            }
            for (std::size_t target = 0; target < targetCount; ++target)
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
            m_recordedPool = pool;
            m_recordedChosen = reply.chosen;
            m_recordedOutcomes = reply.outcomes;
            for (std::size_t shooter = 0; shooter < pool.size(); ++shooter)
            {
                m_shooterLoads[pool[shooter]].oneActionHpSteps = run.poolHp[shooter];
                if (reply.chosen[shooter] != Coordinator::NO_ONE_ACTION_TARGET)
                {
                    m_shooterLoads[pool[shooter]].oneActionUnit = m_replyEntries[reply.chosen[shooter]].unitId;
                }
            }
        }
        m_oneActionNanos = Coordinator::nanosSince(started);
    }

    const Coordinator::AssignmentActor* DangerAuditRecorder::actorOf(std::int32_t engineUnitId) const
    {
        for (const Coordinator::AssignmentActor & actor : m_view.actors)
        {
            if (actor.engineUnitId == engineUnitId)
            {
                return &actor;
            }
        }
        return nullptr;
    }

    std::vector<Coordinator::ReplyLoss> DangerAuditRecorder::replyLosses(
        std::span<const ReplyEntry> entries, const Coordinator::OneActionReply & reply) const
    {
        std::vector<Coordinator::ReplyLoss> losses;
        losses.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const ReplyEntry & entry = entries[index];
            const Coordinator::ReplyLossFacts facts{
                .actorCost = entry.cost,
                .bestNextShot = entry.pCandidate == nullptr
                                    ? 0
                                    : Coordinator::bestShot(entry.pCandidate->actorNextShotsAtDestination),
                .capture = entry.capture,
            };
            losses.push_back(Coordinator::replyLoss(facts, Coordinator::ceilHpSteps(entry.hp), reply.outcomes[index]));
        }
        return losses;
    }

    // An unmatched fire keeps only its engine target, so that target's knowledge index stands in for the bundle's.
    void DangerAuditRecorder::addSlotFacts(InstalledReply & installed) const
    {
        const auto slotCount = static_cast<std::size_t>(m_view.plan.actionCount());
        installed.slotTargets.resize(slotCount);
        installed.slotPartners.resize(slotCount);
        installed.slotDestinations.resize(slotCount);
        for (std::size_t slot = 0; slot < slotCount; ++slot)
        {
            const Coordinator::PlannedAction & action = m_view.plan.action(static_cast<std::int32_t>(slot));
            if (Coordinator::isLiveState(action.state))
            {
                installed.slotDestinations[slot] = action.destination;
            }
            const Coordinator::CandidateBundle* pCandidate = candidateFor(action);
            if (pCandidate != nullptr)
            {
                const std::vector<std::int32_t> targets = Coordinator::fireTargetsOf(pCandidate->bundle);
                installed.slotTargets[slot].insert(targets.begin(), targets.end());
                installed.slotPartners[slot] = Coordinator::servicedPartnersOf(pCandidate->bundle);
                continue;
            }
            const std::int32_t target = action.targetUnitId == Coordinator::NO_UNIT
                                            ? Coordinator::NO_UNIT
                                            : knowledgeIndexOf(action.targetUnitId);
            if (target != Coordinator::NO_UNIT)
            {
                installed.slotTargets[slot].insert(target);
            }
        }
    }

    // Another entry's shooters keep their facts unless the moved unit changes the free tile they would fire from.
    bool DangerAuditRecorder::firingTilesMove(std::span<const ReplyEntry> entries, std::size_t changed,
                                              const Coordinator::PlannedOccupancy & occupancy) const
    {
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            if (index == changed)
            {
                continue;
            }
            for (const Shooter & shooter : entries[index].shooters)
            {
                if (Coordinator::replyFiringTile(*shooter.pFacts, occupancy) != shooter.firingTile)
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool DangerAuditRecorder::chainInteraction(const InstalledReply & installed, qsizetype planSlot,
                                               const Coordinator::CandidateBundle & installedCandidate,
                                               const Coordinator::CandidateBundle & replacement) const
    {
        for (std::size_t slot = 0; slot < installed.slotTargets.size(); ++slot)
        {
            if (static_cast<qsizetype>(slot) != planSlot &&
                (Coordinator::firesOnAny(installedCandidate.bundle, installed.slotTargets[slot]) ||
                 Coordinator::firesOnAny(replacement.bundle, installed.slotTargets[slot])))
            {
                return true;
            }
        }
        return false;
    }

    // JointPlanStockValuer::rowOf finds the knowledge index through the same unit links knowledgeIndexOf reads.
    bool DangerAuditRecorder::entersStockKey(const Coordinator::PlannedAction & action,
                                             std::int32_t knowledgeIndex) const
    {
        return Coordinator::isLiveState(action.state) &&
               m_view.propertyStock.ourRowOf(knowledgeIndex) != Coordinator::NO_STOCK_ROW;
    }

    bool DangerAuditRecorder::stockKeyUnchangedFor(const Coordinator::PlannedAction & action,
                                                   std::int32_t knowledgeIndex,
                                                   const Coordinator::CandidateBundle & replacement) const
    {
        const bool replacementCaptures = Coordinator::capturesInPlan(Coordinator::planBundleKindOf(replacement.bundle));
        return Coordinator::stockKeyUnchanged(entersStockKey(action, knowledgeIndex), action.destination,
                                              Coordinator::capturesInPlan(action.kind), replacement.bundle.destination,
                                              replacementCaptures);
    }

    // planRecord's reply facts for the replacement, never asking the engine for an enemy shot.
    ReplyEntry DangerAuditRecorder::changedEntry(const ReplyEntry & installed,
                                                 const Coordinator::CandidateBundle & replacement,
                                                 const Coordinator::ActionHpOutcome & planOutcome,
                                                 const Coordinator::PlannedOccupancy & occupancy)
    {
        const Coordinator::ActionHpOutcome isolated =
            Coordinator::hpAfterAction(replacement.bundle, replacement.actor.hpSteps);
        ReplyEntry entry{
            .planSlot = installed.planSlot,
            .unitId = installed.unitId,
            .hp = Coordinator::realHpAfterAction(installed.pActor->getHp(), planOutcome),
            .cost = replacement.actor.replacementCost,
            .knowledgeIndex = installed.knowledgeIndex,
            .pActor = installed.pActor,
            .pCandidate = &replacement,
            .actorHpSteps = replacement.actor.hpSteps,
            .destination = replacement.bundle.destination,
        };
        entry.listsShooters = isolated.hpSteps > 0 || planOutcome.hpSteps > 0;
        if (entry.listsShooters)
        {
            entry.shooters = shootersAmong(occupancy, entry.destination, isolated.victimIndex, isolated.victimHpSteps);
        }
        entry.damage =
            damageOn(entry.shooters, entry.knowledgeIndex, *entry.pActor, entry.destination, ExactQuery::Skipped);
        const std::int32_t propertyIndex = m_view.propertyStock.propertyIndexAt(entry.destination);
        if (Coordinator::capturesInPlan(Coordinator::planBundleKindOf(replacement.bundle)) &&
            propertyIndex != Coordinator::NO_PROPERTY_INDEX)
        {
            const CaptureSite site = captureSite(propertyIndex, entry.knowledgeIndex, *entry.pActor, entry.destination);
            entry.capture = Coordinator::CaptureStake{
                .column = site.column,
                .input = walkInput(*entry.pActor, site.carried, site.rate),
            };
        }
        return entry;
    }

    std::vector<ReplyEntry> DangerAuditRecorder::entriesOn(const Coordinator::PlanChainRun & chain) const
    {
        std::vector<ReplyEntry> entries = m_replyEntries;
        for (ReplyEntry & entry : entries)
        {
            entry.hp = Coordinator::realHpAfterAction(entry.pActor->getHp(),
                                                      chain.outcomes[static_cast<std::size_t>(entry.planSlot)]);
        }
        return entries;
    }

    PricedChange DangerAuditRecorder::priceChange(const InstalledReply & installed, std::size_t entryIndex,
                                                  Coordinator::ChangeReason reason, std::size_t candidateIndex,
                                                  const Coordinator::CandidateBundle & replacement)
    {
        const ReplyEntry & entry = m_replyEntries[entryIndex];
        const auto slot = static_cast<std::size_t>(entry.planSlot);
        PricedChange priced;
        if (Coordinator::destinationTaken(installed.slotDestinations, slot, replacement.bundle.destination))
        {
            priced.omission = Coordinator::ChangeOmission::DestinationTaken;
            return priced;
        }
        std::vector<Coordinator::PlanChainEntry> chainEntries = installed.chainEntries;
        chainEntries[slot] = Coordinator::PlanChainEntry{.pBundle = &replacement.bundle,
                                                         .actorHpSteps = replacement.actor.hpSteps};
        const Coordinator::PlanChainRun chain = Coordinator::planChain(chainEntries);
        priced.omission = Coordinator::chainOmission(installed.chain, chain, slot);
        if (priced.omission != Coordinator::ChangeOmission::None)
        {
            return priced;
        }
        const Coordinator::PlannedOccupancy occupancy = plannedOccupancy(entry.unitId, replacement.bundle.destination);
        std::vector<ReplyEntry> entries = entriesOn(chain);
        entries[entryIndex] = changedEntry(entry, replacement, chain.outcomes[slot], occupancy);
        const Coordinator::PlannedAction & action = m_view.plan.action(static_cast<std::int32_t>(entry.planSlot));
        const Coordinator::ChangeFlags flags{
            .stockKeyUnchanged = stockKeyUnchangedFor(action, entry.knowledgeIndex, replacement),
            .chainInteraction = chainInteraction(installed, entry.planSlot, *entry.pCandidate, replacement),
            .firingTilesShared =
                replacement.bundle.destination != entry.destination && firingTilesMove(entries, entryIndex, occupancy),
            .serviceInteraction = Coordinator::serviceInteraction(entry.pCandidate->bundle, replacement.bundle,
                                                                  installed.slotPartners, slot, entry.knowledgeIndex),
        };
        priced.exact = Coordinator::exactChange(flags);
        const Coordinator::PlanReplyRun run = oneActionRun(entries, chain.chain, LOAD_SOURCE);
        priced.pairsExamined = run.reply.pairsExamined;
        const Coordinator::ReplyLoss total = Coordinator::totalLoss(replyLosses(entries, run.reply));
        const Coordinator::BundleValuation & before = entry.pCandidate->valuation;
        const Coordinator::BundleValuation & after = replacement.valuation;
        const Coordinator::MilliFunds seatedDelta = Coordinator::seatedValue(after) - Coordinator::seatedValue(before);
        priced.gain = Coordinator::changeGain(seatedDelta, installed.total, total);
        const Coordinator::ReplyOutcome & outcome = run.reply.outcomes[entryIndex];
        const std::vector<std::int32_t> targets = Coordinator::fireTargetsOf(replacement.bundle);
        const std::int32_t target = targets.empty()
                                        ? Coordinator::NO_UNIT
                                        : Coordinator::linkOf(m_view.unitLinks, targets.front()).engineUnitId;
        priced.record = QJsonObject{
            {KEY_UNIT, entry.unitId},
            {KEY_SLOT, static_cast<qint64>(entry.planSlot)},
            {KEY_REASON, QLatin1String(reasonName(reason))},
            {KEY_CANDIDATE, static_cast<qint64>(candidateIndex)},
            {KEY_KIND, Coordinator::traceBundleKind(Coordinator::planBundleKindOf(replacement.bundle))},
            {KEY_DESTINATION, tileRecord(replacement.bundle.destination)},
            {KEY_TARGET, target == Coordinator::NO_UNIT ? QJsonValue() : QJsonValue(target)},
            {KEY_STOCK_KEY_UNCHANGED, flags.stockKeyUnchanged},
            {KEY_CHAIN_INTERACTION, flags.chainInteraction},
            {KEY_FIRING_TILES_SHARED, flags.firingTilesShared},
            {KEY_SERVICE_INTERACTION, flags.serviceInteraction},
            {KEY_EXACT, priced.exact},
            {KEY_PLANNER_DELTA, static_cast<qint64>(after.value().economicValue - before.value().economicValue)},
            {KEY_TARGET_CONTINUATION_DELTA, static_cast<qint64>(after.continuation.targetContinuationRemoved -
                                                                before.continuation.targetContinuationRemoved)},
            {KEY_SEATED_DELTA, static_cast<qint64>(seatedDelta)},
            {KEY_REPLY_DELTA, lossTotalRecord(total - installed.total)},
            {KEY_GAIN, static_cast<qint64>(priced.gain)},
            {Record::KEY_HP_AFTER_REPLY, outcome.hpSteps},
            {Record::KEY_DESTROYED, outcome.destroyed},
        };
        return priced;
    }

    InstalledReply DangerAuditRecorder::installedReply() const
    {
        InstalledReply installed;
        addSlotFacts(installed);
        installed.chainEntries.resize(installed.slotTargets.size());
        for (std::size_t slot = 0; slot < installed.chainEntries.size(); ++slot)
        {
            installed.chainEntries[slot].firedTargets.assign(installed.slotTargets[slot].begin(),
                                                             installed.slotTargets[slot].end());
        }
        for (const ReplyEntry & entry : m_replyEntries)
        {
            Coordinator::PlanChainEntry & chainEntry = installed.chainEntries[static_cast<std::size_t>(entry.planSlot)];
            chainEntry.actorHpSteps = entry.actorHpSteps;
            if (entry.pCandidate != nullptr)
            {
                chainEntry.pBundle = &entry.pCandidate->bundle;
            }
        }
        installed.chain = Coordinator::planChain(installed.chainEntries);
        installed.entries = entriesOn(installed.chain);
        installed.run = oneActionRun(installed.entries, installed.chain.chain, LOAD_SOURCE);
        installed.losses = replyLosses(installed.entries, installed.run.reply);
        installed.total = Coordinator::totalLoss(installed.losses);
        return installed;
    }

    // The baseline is rebuilt the way changes are; it must reproduce the P1 outcome the plan records.
    bool DangerAuditRecorder::baselineConsistent(const InstalledReply & installed) const
    {
        if (installed.chain.chain != m_planChain || installed.run.pool != m_recordedPool ||
            installed.run.reply.chosen != m_recordedChosen || installed.run.reply.outcomes != m_recordedOutcomes)
        {
            return false;
        }
        for (std::size_t index = 0; index < m_replyEntries.size(); ++index)
        {
            if (installed.entries[index].hp != m_replyEntries[index].hp)
            {
                return false;
            }
        }
        return true;
    }

    // Runs once the record's other keys are final, so its unit lookups cannot change their counts.
    QJsonObject DangerAuditRecorder::replyPricingRecord()
    {
        const auto started = std::chrono::steady_clock::now();
        const InstalledReply installed = installedReply();
        std::set<std::int32_t> firing;
        for (std::size_t shooter = 0; shooter < installed.run.pool.size(); ++shooter)
        {
            if (installed.run.reply.chosen[shooter] != Coordinator::NO_ONE_ACTION_TARGET)
            {
                firing.insert(installed.run.pool[shooter]);
            }
        }
        Coordinator::MilliFunds exposureCharged = 0;
        Coordinator::MilliFunds targetContinuationCharged = 0;
        std::int32_t unitsShot = 0;
        std::int32_t unitsDestroyed = 0;
        std::int32_t unmatchedEntries = 0;
        std::int64_t changesPriced = 0;
        std::int64_t pairsExamined = 0;
        ChangeBucket bestExact;
        ChangeBucket bestOpen;
        QJsonArray units;
        for (std::size_t index = 0; index < installed.entries.size(); ++index)
        {
            const ReplyEntry & entry = installed.entries[index];
            const std::int32_t hpBefore = Coordinator::ceilHpSteps(entry.hp);
            const Coordinator::ReplyOutcome & outcome = installed.run.reply.outcomes[index];
            const bool shot = outcome.hpSteps < hpBefore;
            unitsShot += shot ? 1 : 0;
            unitsDestroyed += hpBefore > 0 && outcome.destroyed ? 1 : 0;
            if (entry.pCandidate == nullptr)
            {
                ++unmatchedEntries;
            }
            else
            {
                exposureCharged -= entry.pCandidate->valuation.continuation.exposure;
                targetContinuationCharged += entry.pCandidate->valuation.continuation.targetContinuationRemoved;
            }
            const Coordinator::AssignmentActor* pActor = actorOf(entry.unitId);
            std::span<const Coordinator::CandidateBundle> candidates;
            if (entry.pCandidate != nullptr && pActor != nullptr)
            {
                candidates = pActor->candidates;
            }
            const Coordinator::ChangeReason reason =
                shot ? Coordinator::ChangeReason::Shot : Coordinator::ChangeReason::HitsShooter;
            std::int32_t considered = 0;
            std::int32_t invalid = 0;
            std::int32_t priced = 0;
            std::map<Coordinator::ChangeOmission, std::int32_t> omitted;
            ChangeBucket exact;
            ChangeBucket open;
            for (std::size_t candidate = 0; candidate < candidates.size(); ++candidate)
            {
                const Coordinator::CandidateBundle & replacement = candidates[candidate];
                if (&replacement == entry.pCandidate || !Coordinator::consideredFor(reason, replacement.bundle, firing))
                {
                    continue;
                }
                if (!replacement.valuation.valid)
                {
                    ++invalid;
                    continue;
                }
                ++considered;
                const PricedChange change = priceChange(installed, index, reason, candidate, replacement);
                if (change.omission != Coordinator::ChangeOmission::None)
                {
                    ++omitted[change.omission];
                    continue;
                }
                ++priced;
                pairsExamined += change.pairsExamined;
                (change.exact ? exact : open).add(change.gain, change.record);
            }
            if (!shot && considered + invalid == 0)
            {
                continue;
            }
            changesPriced += priced;
            bestExact.offerBest(exact);
            bestOpen.offerBest(open);
            QJsonObject omittedRecord;
            for (const Coordinator::ChangeOmission omission : OMISSIONS)
            {
                omittedRecord.insert(omissionName(omission), omitted[omission]);
            }
            units.append(QJsonObject{
                {KEY_UNIT, entry.unitId},
                {KEY_SLOT, static_cast<qint64>(entry.planSlot)},
                {KEY_REASON, QLatin1String(reasonName(reason))},
                {KEY_HP_BEFORE, hpBefore},
                {Record::KEY_HP_AFTER_REPLY, outcome.hpSteps},
                {Record::KEY_DESTROYED, outcome.destroyed},
                {KEY_LOSS, lossRecord(installed.losses[index])},
                {KEY_CANDIDATES, considered},
                {KEY_INVALID, invalid},
                {KEY_HAS_STOCK_ROW,
                 entersStockKey(m_view.plan.action(static_cast<std::int32_t>(entry.planSlot)), entry.knowledgeIndex)},
                {KEY_PRICED, priced},
                {KEY_OMITTED, omittedRecord},
                {KEY_EXACT, bucketRecord(exact)},
                {KEY_OPEN, bucketRecord(open)},
            });
        }
        QJsonObject installedRecord = lossTotalRecord(installed.total);
        installedRecord.insert(KEY_EXPOSURE_CHARGED, static_cast<qint64>(exposureCharged));
        installedRecord.insert(KEY_TARGET_CONTINUATION_CHARGED, static_cast<qint64>(targetContinuationCharged));
        installedRecord.insert(KEY_UNITS_SHOT, unitsShot);
        installedRecord.insert(KEY_UNITS_DESTROYED, unitsDestroyed);
        return QJsonObject{
            {KEY_SOURCE, QLatin1String(sourceName(LOAD_SOURCE))},
            {KEY_NEXT_SHOT_SCALED, true},
            {KEY_BASELINE_CONSISTENT, baselineConsistent(installed)},
            {KEY_INSTALLED, installedRecord},
            {KEY_UNITS, units},
            {KEY_BEST_EXACT, bestRecord(bestExact)},
            {KEY_BEST_OPEN, bestRecord(bestOpen)},
            {KEY_CHANGES_PRICED, static_cast<qint64>(changesPriced)},
            {KEY_PAIRS_EXAMINED, static_cast<qint64>(pairsExamined)},
            {KEY_INSTALLED_PAIRS_EXAMINED, static_cast<qint64>(installed.run.reply.pairsExamined)},
            {KEY_UNMATCHED_ENTRIES, unmatchedEntries},
            {KEY_NANOS, static_cast<qint64>(Coordinator::nanosSince(started))},
        };
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
        QJsonObject result{
            {KEY_SCHEMA, DANGER_AUDIT_SCHEMA},
            {KEY_DAY, m_view.identity.day},
            {KEY_PLAYER, m_view.identity.playerId},
            {KEY_PLAN_SEQUENCE, static_cast<qint64>(m_view.identity.planSequence)},
            {KEY_HORIZON, m_horizonTurns},
            {KEY_POINTS_TO_CAPTURE, Unit::MAX_CAPTURE_POINTS},
            {KEY_TERRAIN_LUCK, Coordinator::REPLY_TERRAIN_LUCK},
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
            {KEY_NANOS, static_cast<qint64>(Coordinator::nanosSince(started))},
        };
        result.insert(KEY_REPLY_PRICING, replyPricingRecord());
        result.insert(KEY_PLAN_REPLAY, planReplayRecord());
        result.insert(KEY_STOCK_REPLY_QUOTE, stockReplyQuoteRecord(plan));
        return result;
    }

    Record::StockPlanMeasurement DangerAuditRecorder::stockPlanMeasurement()
    {
        using namespace Coordinator;
        if (!orderingAcyclic(m_view.plan.executionOrder()))
        {
            return {.status = Record::StockQuoteStatus::CyclicOrdering};
        }
        const auto count = static_cast<std::size_t>(m_view.plan.actionCount());
        std::vector<PlanChainEntry> entries(count);
        std::vector<ReplyTargetFacts> targets(count);
        std::vector<PlanReplyEntry> replies(count);
        for (std::size_t slot = 0; slot < count; ++slot)
        {
            entries[slot].actorHpSteps = Record::UNKNOWN_ACTION_HP;
            if (!isLiveState(m_view.plan.action(static_cast<std::int32_t>(slot)).state))
            {
                continue;
            }
            const auto old = std::find_if(m_replyEntries.begin(), m_replyEntries.end(),
                [slot](const ReplyEntry & entry) { return entry.planSlot == static_cast<qsizetype>(slot); });
            if (old == m_replyEntries.end())
            {
                continue;
            }
            const auto facts = m_facts.factsFor(old->knowledgeIndex, *old->pActor, old->destination);
            entries[slot] = PlanChainEntry{old->pCandidate == nullptr ? nullptr : &old->pCandidate->bundle,
                                           known(old->knowledgeIndex).hpRounded};
            targets[slot].target = OneActionTarget{old->unitId, facts->actorRealHp, old->cost};
            auto & reply = replies[slot];
            reply.pFacts = facts.get();
            reply.destination = old->destination;
        }
        return Record::measureStockPlan(m_view.plan, entries, targets,
            [&](const Record::StockPlanMeasurement & measurement, std::span<ReplyTargetFacts> orderedTargets)
            {
                prepareStockReply(measurement, replies, orderedTargets);
            },
            [&](std::int32_t shooter) { return known(shooter).hpRounded; },
            [&](std::int32_t target, std::size_t slot, std::int32_t shooterHp, std::int32_t hp)
            {
                return pricedTerrainDamage(replies[target].pFacts->terrain, replies[target].shots[slot], shooterHp, hp);
            });
    }

    void DangerAuditRecorder::prepareStockReply(const Record::StockPlanMeasurement & measurement,
                                                std::vector<Coordinator::PlanReplyEntry> & replies,
                                                std::span<Coordinator::ReplyTargetFacts> orderedTargets)
    {
        using namespace Coordinator;
        const auto occupancy = Record::stockReplyOccupancy(
            plannedOccupancy(NO_UNIT, INVALID_TILE, PlanOccupancyView::Live), measurement,
            [&](std::int32_t slot) { return knowledgeIndexOf(m_view.plan.action(slot).unitId); });
        for (std::size_t index = 0; index < measurement.planSlots.size(); ++index)
        {
            if (orderedTargets[index].target.hp <= 0)
            {
                continue;
            }
            auto & reply = replies[measurement.planSlots[index]];
            const auto & facts = *reply.pFacts;
            for (std::size_t shooter = 0; shooter < facts.tile->shooters.size(); ++shooter)
            {
                const auto & source = facts.tile->shooters[shooter];
                const auto firingTile = replyFiringTile(source, occupancy);
                reply.shots.push_back(resolveReplyShot(source, facts.shots[shooter], source.dayStartHpSteps,
                                                       firingTile, reply.destination));
                orderedTargets[index].shooters.push_back(replySlotFacts(source, reply.shots.back()));
            }
        }
    }

    void addCounterRecord(QJsonObject & record, const Record::MeasuredCounter & counter)
    {
        record.insert(KEY_ISOLATED_COUNTER_STEPS, counter.isolatedSteps);
        record.insert(KEY_COUNTER_STEPS_USED, counter.steps);
        record.insert(KEY_COUNTER_SCALED, counter.counterScaled);
    }

    QJsonObject DangerAuditRecorder::stockReplyQuoteRecord(const QJsonArray & plan)
    {
        using namespace Coordinator;
        const auto intervalRecord = [](const StockInterval & interval)
        {
            return QJsonArray{static_cast<qint64>(interval.lower), static_cast<qint64>(interval.upper)};
        };
        const auto keyRecord = [](const ContinuationKey & key)
        {
            QJsonArray encoded;
            for (const auto & action : key)
            {
                encoded.append(QJsonArray{action.row(), action.x(), action.y(), action.captures(),
                                           action.stepsLost(), action.destroyed()});
            }
            return encoded;
        };
        ContinuationKey today;
        ContinuationKey afterOwnActions;
        ContinuationKey afterReply;
        QJsonArray units;
        std::int32_t diedAttacking = 0;
        std::int32_t killedByReply = 0;
        std::int32_t damagedAlive = 0;
        const bool fieldAvailable = m_view.propertyStock.horizonTurns() > 0 &&
                                    m_view.propertyStock.capturePointsToCapture() > 0;
        const auto chainStarted = std::chrono::steady_clock::now();
        const auto measurement = fieldAvailable ? stockPlanMeasurement() :
            Record::StockPlanMeasurement{.status = Record::StockQuoteStatus::MissingField};
        const auto chainNanos = Coordinator::nanosSince(chainStarted);
        bool chainUsable = measurement.status == Record::StockQuoteStatus::Available;
        QJsonArray ownActions;
        std::int32_t ownDifferences = 0;
        std::int32_t replyDifferences = 0;
        std::int32_t ownComparisons = 0;
        std::int32_t replyComparisons = 0;
        for (std::size_t index = 0; index < measurement.planSlots.size(); ++index)
        {
            const auto slot = measurement.planSlots[index];
            const auto & action = m_view.plan.action(slot);
            const bool knownOwn = index < measurement.ownStatus.size() &&
                measurement.ownStatus[index] == Record::StockQuoteStatus::Available;
            const auto status = knownOwn ? Record::StockQuoteStatus::Available :
                index < measurement.ownStatus.size() ? measurement.ownStatus[index] : measurement.status;
            QJsonObject ownAction{
                {KEY_UNIT, action.unitId}, {KEY_SLOT, slot}, {KEY_OWN_ACTION_KNOWN, knownOwn},
                {KEY_REASON, QLatin1String(Record::stockQuoteStatusName(status))},
                {KEY_HP_AFTER_ACTION, knownOwn ? QJsonValue(measurement.chain.outcomes[index].hpSteps) : QJsonValue()},
                {KEY_HP_REAL_AFTER_ACTION, knownOwn ? QJsonValue(measurement.hpAfterActions[index]) : QJsonValue()},
            };
            if (index < measurement.counters.size())
            {
                addCounterRecord(ownAction, measurement.counters[index]);
            }
            ownActions.append(ownAction);
            const auto legacy = plan.at(slot).toObject();
            if (knownOwn && legacy.contains(KEY_HP_AFTER_ACTION_PLAN))
            {
                ++ownComparisons;
                ownDifferences += measurement.chain.outcomes[index].hpSteps != legacy.value(KEY_HP_AFTER_ACTION_PLAN).toInt() ||
                    (legacy.contains(KEY_HP_REAL_AFTER_ACTION_PLAN) && measurement.hpAfterActions[index] !=
                        legacy.value(KEY_HP_REAL_AFTER_ACTION_PLAN).toDouble()) ? 1 : 0;
            }
            const auto old = std::find_if(m_replyEntries.begin(), m_replyEntries.end(),
                [slot](const ReplyEntry & entry) { return entry.planSlot == slot; });
            if (chainUsable && old != m_replyEntries.end())
            {
                const auto oldIndex = static_cast<std::size_t>(old - m_replyEntries.begin());
                if (oldIndex < m_recordedOutcomes.size())
                {
                    ++replyComparisons;
                    replyDifferences += measurement.reply.reply.outcomes[index] != m_recordedOutcomes[oldIndex] ? 1 : 0;
                }
            }
        }
        for (std::size_t index = 0; index < measurement.planSlots.size() && chainUsable; ++index)
        {
            const auto slot = measurement.planSlots[index];
            const auto & action = m_view.plan.action(slot);
            const auto knowledge = knowledgeIndexOf(action.unitId);
            const auto row = m_view.propertyStock.ourRowOf(knowledge);
            if (row == NO_STOCK_ROW)
            {
                continue;
            }
            const auto & outcome = measurement.reply.reply.outcomes[index];
            const auto dayStartSteps = known(knowledge).hpRounded;
            const auto ownHp = measurement.chain.outcomes[index].hpSteps;
            const auto counterSteps = dayStartSteps - ownHp;
            const auto replySteps = ownHp - outcome.hpSteps;
            const auto key = CanonicalPlanActionKey::fromAction(row, action.destination.x, action.destination.y,
                                                               capturesInPlan(action.kind));
            const auto variants = Record::stockReplyKeys(key, dayStartSteps, counterSteps, replySteps);
            if (!variants)
            {
                chainUsable = false;
                break;
            }
            today.push_back(key);
            afterOwnActions.push_back(variants->afterOwnActions);
            afterReply.push_back(variants->afterReply);
            if (counterSteps == 0 && replySteps == 0 && variants->death == Record::StockDeathCause::None)
            {
                continue;
            }
            diedAttacking += variants->death == Record::StockDeathCause::OwnAction ? 1 : 0;
            killedByReply += variants->death == Record::StockDeathCause::Reply ? 1 : 0;
            damagedAlive += variants->death == Record::StockDeathCause::None ? 1 : 0;
            QJsonObject unit{
                {KEY_UNIT, action.unitId}, {KEY_ROW, row},
                {KEY_COUNTER_STEPS, counterSteps}, {KEY_REPLY_STEPS, replySteps},
                {KEY_DEATH_CAUSE, QLatin1String(Record::STOCK_DEATH_CAUSE_NAMES[static_cast<std::size_t>(variants->death)])},
                {KEY_COMPLETED_CAPTURE, m_view.propertyStock.actionCompletesCapture(
                    PlanRowAction{row, action.destination, capturesInPlan(action.kind)})},
            };
            addCounterRecord(unit, measurement.counters[index]);
            units.append(unit);
        }
        std::sort(today.begin(), today.end());
        std::sort(afterOwnActions.begin(), afterOwnActions.end());
        std::sort(afterReply.begin(), afterReply.end());
        const std::array keys{today, afterOwnActions, afterReply};
        DiagnosticStockQuotes quotes;
        bool replayValid = false;
        if (fieldAvailable && chainUsable)
        {
            quotes = JointPlanStockValuer::diagnosticQuotes(m_view.propertyStock, keys);
            replayValid = quotes.quotes.size() == keys.size() &&
                std::all_of(quotes.quotes.begin(), quotes.quotes.end(),
                    [](const DiagnosticStockQuote & quote) { return quote.valid && quote.witnessReplays; });
        }
        const auto status = fieldAvailable && measurement.status != Record::StockQuoteStatus::Available
            ? measurement.status : Record::stockQuoteStatus(fieldAvailable, chainUsable, replayValid);
        QJsonObject result{
            {KEY_AVAILABLE, status == Record::StockQuoteStatus::Available},
            {KEY_REASON, QLatin1String(Record::stockQuoteStatusName(status))},
            {KEY_OWN_ACTIONS, ownActions},
            {KEY_CERTAIN_COUNTER_UNITS, measurement.certainCounterUnits},
            {KEY_SCALED_COUNTER_UNITS, measurement.scaledCounterUnits},
            {KEY_SCALED_COUNTER_SURVIVAL_DIFFERENCES, measurement.scaledCounterSurvivalDifferences},
            {KEY_OWN_HP_DIFFERENCES, ownDifferences}, {KEY_REPLY_OUTCOME_DIFFERENCES, replyDifferences},
            {KEY_OWN_HP_COMPARISONS, ownComparisons}, {KEY_REPLY_OUTCOME_COMPARISONS, replyComparisons},
            {KEY_CHAIN_NANOS, static_cast<qint64>(chainNanos)},
            {KEY_PREPARATION_NANOS, static_cast<qint64>(quotes.preparationNanos)},
            {KEY_UNITS, units}, {KEY_DIED_ATTACKING, diedAttacking},
            {KEY_KILLED_BY_REPLY, killedByReply}, {KEY_DAMAGED_ALIVE, damagedAlive},
            {KEY_ACTIONS, static_cast<qint64>(quotes.actions)},
            {KEY_CLASSES_BUILT, static_cast<qint64>(quotes.classesBuilt)},
            {KEY_NANOS, static_cast<qint64>(quotes.nanos)},
        };
        const std::array names{KEY_TODAY, KEY_AFTER_OWN_ACTIONS, KEY_AFTER_REPLY};
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            QJsonObject quoteRecord{{KEY_KEY, keyRecord(keys[index])}, {KEY_VALID, false},
                                    {KEY_WITNESS_REPLAYS, false}};
            if (index < quotes.quotes.size())
            {
                const auto & quote = quotes.quotes[index];
                quoteRecord.insert(KEY_REASON, QLatin1String(DIAGNOSTIC_STOCK_STATUS_NAMES[static_cast<std::size_t>(quote.status)]));
                quoteRecord.insert(KEY_PRICING_NANOS, static_cast<qint64>(quote.pricingNanos));
                quoteRecord.insert(KEY_REPLAY_NANOS, static_cast<qint64>(quote.replayNanos));
                quoteRecord.insert(KEY_VALID, quote.valid);
                quoteRecord.insert(KEY_WITNESS_REPLAYS, quote.witnessReplays);
                if (quote.valid)
                {
                    quoteRecord.insert(KEY_OURS, intervalRecord(quote.ours));
                    quoteRecord.insert(KEY_OWNED, static_cast<qint64>(quote.owned));
                }
            }
            result.insert(names[index], quoteRecord);
        }
        if (replayValid)
        {
            result.insert(KEY_COUNTER_LOSS, intervalRecord(Record::stockQuoteLoss(
                quotes.quotes[0].ours, quotes.quotes[1].ours)));
            result.insert(KEY_REPLY_LOSS, intervalRecord(Record::stockQuoteLoss(
                quotes.quotes[1].ours, quotes.quotes[2].ours)));
        }
        return result;
    }

    std::int32_t DangerAuditRecorder::chainHp(const Coordinator::TargetHpChain & chain, std::int32_t shooterIndex) const
    {
        const auto damaged = chain.find(shooterIndex);
        return damaged == chain.end() ? known(shooterIndex).hpRounded : damaged->second.after;
    }

    std::map<std::int32_t, std::int32_t> DangerAuditRecorder::poolHp(const std::set<std::int32_t> & shooters,
                                                                     const Coordinator::TargetHpChain & chain) const
    {
        std::map<std::int32_t, std::int32_t> hp;
        for (const std::int32_t shooter : shooters)
        {
            hp.emplace(shooter, chainHp(chain, shooter));
        }
        return hp;
    }

    // Live actions only, in execution order, each matched to its candidate the way planRecord matches it.
    std::vector<Coordinator::PlanReplyAction> DangerAuditRecorder::planReplyActions(
        const Coordinator::OrderingResult & ordering) const
    {
        std::vector<Coordinator::PlanReplyAction> actions;
        actions.reserve(ordering.order.size());
        for (const std::int32_t index : ordering.order)
        {
            const Coordinator::PlannedAction & action = m_view.plan.action(index);
            actions.push_back(Coordinator::PlanReplyAction{
                .pCandidate = candidateFor(action),
                .engineUnitId = action.unitId,
                .actionIndex = index,
                .plannedDamageSteps = action.plannedDamageSteps,
            });
        }
        return actions;
    }

    Coordinator::ReplyBoard DangerAuditRecorder::replyBoard() const
    {
        return Coordinator::ReplyBoard{
            .width = m_view.knowledge.width(),
            .height = m_view.knowledge.height(),
            .standing = m_standing,
            .alliedOccupants = m_alliedOccupants,
        };
    }

    ReplyPairing DangerAuditRecorder::pairEntries(const Coordinator::ResolvedPlanReply & resolved) const
    {
        ReplyPairing pairing{
            .auditOf = std::vector<std::optional<std::size_t>>(resolved.entries.size()),
            .plannerOf = std::vector<std::optional<std::size_t>>(m_replyEntries.size()),
        };
        for (std::size_t audit = 0; audit < m_replyEntries.size(); ++audit)
        {
            const auto found = std::find_if(resolved.entries.begin(), resolved.entries.end(),
                                            [&](const Coordinator::PlanReplyEntry & entry)
                                            {
                                                return entry.engineUnitId == m_replyEntries[audit].unitId;
                                            });
            if (found != resolved.entries.end())
            {
                const auto planner = static_cast<std::size_t>(found - resolved.entries.begin());
                pairing.auditOf[planner] = audit;
                pairing.plannerOf[audit] = planner;
            }
        }
        return pairing;
    }

    // Each model on its own inputs: the planner's isolated hp, granted damage and live-only occupancy.
    std::vector<ReplyCheck> DangerAuditRecorder::firstRunChecks(const Coordinator::ResolvedPlanReply & resolved,
                                                                const Coordinator::PlanReplyRun & run,
                                                                const ReplyPairing & pairing) const
    {
        std::vector<ReplyCheck> checks;
        checks.reserve(resolved.entries.size());
        for (std::size_t index = 0; index < resolved.entries.size(); ++index)
        {
            const Coordinator::PlanReplyEntry & entry = resolved.entries[index];
            std::set<std::int32_t> shooters;
            for (const std::size_t slot : entry.shooterSlots)
            {
                shooters.insert(entry.pFacts->tile->shooters[slot].shooterIndex);
            }
            const std::optional<std::size_t> audit = pairing.auditOf[index];
            if (audit)
            {
                for (const Shooter & shooter : m_replyEntries[*audit].shooters)
                {
                    shooters.insert(shooter.shot.unitIndex);
                }
            }
            const Coordinator::ReplyCheckSide planner{
                .startHp = resolved.targets[index].target.hp,
                .shooterHp = poolHp(shooters, resolved.chain),
                .facts = Coordinator::replyCheckFacts(entry),
                .outcome = run.reply.outcomes[index],
            };
            std::optional<Coordinator::ReplyCheckSide> audited;
            if (audit)
            {
                const ReplyEntry & auditEntry = m_replyEntries[*audit];
                audited = Coordinator::ReplyCheckSide{
                    .startHp = auditEntry.hp,
                    .shooterHp = poolHp(shooters, m_planChain),
                    .facts = auditCheckFacts(auditEntry),
                    .outcome = m_recordedOutcomes[*audit],
                };
            }
            ReplyCheck check{.layer = Coordinator::replyCheckLayer(planner, audited ? &*audited : nullptr)};
            if (check.layer == ReplyCheckLayer::StartHp)
            {
                check.auditHpBefore = audited->startHp;
            }
            if (check.layer == ReplyCheckLayer::EnemyHp)
            {
                check.enemyHp = Coordinator::shooterHpGaps(planner, *audited);
            }
            checks.push_back(std::move(check));
        }
        return checks;
    }

    // Every model input comes from the audit, so only the planner's plain-data facts are left to differ.
    std::vector<std::optional<ReplyCheckLayer>> DangerAuditRecorder::secondRunLayers(
        std::span<const Coordinator::PlanReplyAction> actions, const ReplyPairing & pairing) const
    {
        std::vector<std::optional<ReplyCheckLayer>> layers(pairing.auditOf.size());
        // The audit's outcomes cover all its targets, so a run without some of them proves nothing.
        if (std::ranges::any_of(pairing.plannerOf, [](const auto & planner) { return !planner; }))
        {
            return layers;
        }
        Coordinator::ResolvedPlanReply auditInputs{.complete = true, .chain = m_planChain};
        for (std::size_t audit = 0; audit < m_replyEntries.size(); ++audit)
        {
            const ReplyEntry & entry = m_replyEntries[audit];
            Coordinator::ReplyTargetFacts target;
            auditInputs.entries.push_back(Coordinator::ReplyPredictionDetail::resolveEntry(
                actions[*pairing.plannerOf[audit]], m_occupancy, entry.listsShooters, target, auditInputs.startHp));
            target.target.hp = entry.hp;
            target.target.cost = entry.cost;
            auditInputs.targets.push_back(std::move(target));
        }
        for (auto & [shooter, hp] : auditInputs.startHp)
        {
            hp = known(shooter).hpRounded;
        }
        const Coordinator::PlanReplyRun run = Coordinator::runResolvedPlanReply(auditInputs);
        for (std::size_t audit = 0; audit < m_replyEntries.size(); ++audit)
        {
            layers[*pairing.plannerOf[audit]] = Coordinator::factsThenOutcome(
                Coordinator::replyCheckFacts(auditInputs.entries[audit]), auditCheckFacts(m_replyEntries[audit]),
                run.reply.outcomes[audit], m_recordedOutcomes[audit]);
        }
        return layers;
    }

    // The planner's model of the plan beside the audit's own P1; the audit's model is left as it is.
    QJsonObject DangerAuditRecorder::planReplayRecord() const
    {
        const bool present = m_view.reply.mode == Coordinator::ReplyPrediction::Recorded;
        QJsonObject record{{KEY_PRESENT, present}};
        if (!present)
        {
            return record;
        }
        const auto started = std::chrono::steady_clock::now();
        record.insert(KEY_TELEMETRY, replyTelemetryRecord(m_view.reply));
        record.insert(KEY_AUDIT_ENTRIES, static_cast<qint64>(m_replyEntries.size()));
        const Coordinator::OrderingResult ordering = m_view.plan.executionOrder();
        const std::vector<Coordinator::PlanReplyAction> actions = planReplyActions(ordering);
        const Coordinator::ReplyBoard board = replyBoard();
        Coordinator::ResolvedPlanReply resolved;
        if (Coordinator::orderingAcyclic(ordering))
        {
            resolved = Coordinator::resolvePlanReply(actions, board);
        }
        record.insert(KEY_COMPLETE, resolved.complete);
        if (resolved.complete)
        {
            record.insert(KEY_OCCUPANCY_AGREES,
                          Coordinator::sameOccupancy(Coordinator::planReplyOccupancy(actions, board), m_occupancy,
                                                     board.width, board.height));
            const Coordinator::PlanReplyRun run = Coordinator::runResolvedPlanReply(resolved);
            const Coordinator::PlanReplyPrediction prediction = Coordinator::planReplyPrediction(resolved, run);
            const ReplyPairing pairing = pairEntries(resolved);
            const std::vector<ReplyCheck> checks = firstRunChecks(resolved, run, pairing);
            const std::vector<std::optional<ReplyCheckLayer>> auditInputsLayers = secondRunLayers(actions, pairing);
            QJsonArray entries;
            std::int32_t agrees = 0;
            std::int32_t auditInputsAgree = 0;
            for (std::size_t index = 0; index < prediction.actors.size(); ++index)
            {
                const Coordinator::PlanReplyActorOutcome & actor = prediction.actors[index];
                const ReplyCheck & check = checks[index];
                agrees += check.layer == ReplyCheckLayer::Agrees ? 1 : 0;
                QJsonObject entry{
                    {KEY_UNIT, actor.engineUnitId},
                    {KEY_SLOT, actor.actionIndex},
                    {KEY_HP_BEFORE, actor.hpBefore},
                    {KEY_HP_STEPS_BEFORE, actor.hpStepsBefore},
                    {KEY_STEPS_LOST, actor.stepsLost},
                    {Record::KEY_DESTROYED, actor.destroyed},
                    {KEY_LAYER, layerValue(check.layer)},
                };
                if (check.layer == ReplyCheckLayer::StartHp)
                {
                    entry.insert(KEY_AUDIT_HP_BEFORE, check.auditHpBefore);
                }
                if (check.layer == ReplyCheckLayer::EnemyHp)
                {
                    entry.insert(KEY_ENEMY_HP, enemyHpRecord(check.enemyHp));
                }
                if (auditInputsLayers[index])
                {
                    auditInputsAgree += *auditInputsLayers[index] == ReplyCheckLayer::Agrees ? 1 : 0;
                    entry.insert(KEY_AUDIT_INPUTS_LAYER, layerValue(*auditInputsLayers[index]));
                }
                entries.append(entry);
            }
            QJsonArray auditOnly;
            for (std::size_t audit = 0; audit < m_replyEntries.size(); ++audit)
            {
                if (!pairing.plannerOf[audit])
                {
                    auditOnly.append(m_replyEntries[audit].unitId);
                }
            }
            record.insert(KEY_ENTRIES, entries);
            record.insert(KEY_AGREES, agrees);
            record.insert(KEY_AUDIT_ONLY, auditOnly);
            // Left out when the strict run was skipped, so a skipped run never reads as zero agreement.
            if (auditOnly.isEmpty())
            {
                record.insert(KEY_AUDIT_INPUTS_AGREE, auditInputsAgree);
            }
            record.insert(KEY_PAIRS_EXAMINED, static_cast<qint64>(prediction.pairsExamined));
        }
        record.insert(KEY_NANOS, static_cast<qint64>(Coordinator::nanosSince(started)));
        return record;
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
