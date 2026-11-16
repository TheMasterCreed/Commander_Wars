#include "ai/coordinator/replypassrecord.h"

#include <chrono>
#include <cstddef>
#include <span>

#include <QJsonArray>
#include <QJsonValue>
#include <QLatin1String>

#include "ai/coordinator/capturedangerrecord.h"

namespace
{
    using namespace Coordinator;

    using CaptureDangerRecord::KEY_SCHEMA;
    using CaptureDangerRecord::KEY_DAY;
    using CaptureDangerRecord::KEY_PLAYER;
    using CaptureDangerRecord::KEY_PLAN_SEQUENCE;
    constexpr auto KEY_MODE = "mode";
    constexpr auto KEY_RAN = "ran";
    using CaptureDangerRecord::KEY_REASON;
    constexpr auto KEY_HANDOFF = "handoff";
    constexpr auto KEY_PASS_NANOS = "passNanos";
    constexpr auto KEY_SHOT_NANOS = "shotNanos";
    constexpr auto KEY_VERIFICATION_NANOS = "verificationNanos";
    constexpr auto KEY_PLAN_CHOICES = "planChoices";
    constexpr auto KEY_END = "end";
    constexpr auto KEY_INCUMBENT_NO_VALUE = "incumbentNoValue";
    constexpr auto KEY_BASELINE_STATUS = "baselineStatus";
    constexpr auto KEY_STOCK_CONTEXT_BUILT = "stockContextBuilt";
    constexpr auto KEY_SHOT_UNITS = "shotUnits";
    constexpr auto KEY_SHOT = "shot";
    constexpr auto KEY_CHANGEABLE = "changeable";
    constexpr auto KEY_NOT_CHANGEABLE = "notChangeable";
    constexpr auto KEY_FIXED_ROLE = "fixedRole";
    constexpr auto KEY_COMMITTED = "committed";
    constexpr auto KEY_COUNTERS = "counters";
    using CaptureDangerRecord::KEY_UNITS_SHOT;
    constexpr auto KEY_SHOT_CHANGEABLE = "shotChangeable";
    constexpr auto KEY_SHOT_NOT_CHANGEABLE = "shotNotChangeable";
    constexpr auto KEY_SHOT_FIXED_ROLE = "shotFixedRole";
    constexpr auto KEY_SHOT_COMMITTED = "shotCommitted";
    constexpr auto KEY_LISTED_SHOT = "listedShot";
    constexpr auto KEY_LISTED_HITS_SHOOTER = "listedHitsShooter";
    constexpr auto KEY_NO_VALUE = "noValue";
    constexpr auto KEY_NO_INTERVAL = "noInterval";
    constexpr auto KEY_PROVEN_NOT_BETTER = "provenNotBetter";
    constexpr auto KEY_CANNOT_CLEAR = "cannotClear";
    constexpr auto KEY_FLOORS_BUILT = "floorsBuilt";
    constexpr auto KEY_UNDECIDED = "undecided";
    constexpr auto KEY_RETUNES = "retunes";
    constexpr auto KEY_SWITCHES_AT_FLOOR = "switchesAtFloor";
    constexpr auto KEY_SWITCHES_AT_RETUNE = "switchesAtRetune";
    constexpr auto KEY_ENDED_BY_LIMIT = "endedByLimit";
    constexpr auto KEY_COSTS = "costs";
    using CaptureDangerRecord::KEY_NANOS;
    using CaptureDangerRecord::KEY_WORK;
    constexpr auto KEY_CALLS = "calls";
    constexpr auto KEY_PLANS = "plans";
    constexpr auto KEY_CHOICES = "choices";
    constexpr auto KEY_EXECUTION_ORDER = "executionOrder";
    using CaptureDangerRecord::KEY_AFTER_OWN_ACTIONS;
    using CaptureDangerRecord::KEY_AFTER_REPLY;
    using CaptureDangerRecord::KEY_ACTORS;
    constexpr auto KEY_ACTOR = "actor";
    using CaptureDangerRecord::KEY_UNIT;
    constexpr auto KEY_ACTION = "action";
    using CaptureDangerRecord::KEY_CANDIDATE;
    constexpr auto KEY_GRANTED_DAMAGE = "grantedDamage";
    constexpr auto KEY_DAY_START_STEPS = "dayStartSteps";
    constexpr auto KEY_STEPS_AFTER_OWN_ACTIONS = "stepsAfterOwnActions";
    constexpr auto KEY_STEPS_AFTER_REPLY = "stepsAfterReply";
    constexpr auto KEY_DESTROYED_BY = "destroyedBy";
    constexpr auto KEY_STOCK_ROW = "stockRow";
    constexpr auto KEY_ACTING_SEATED = "actingSeated";
    constexpr auto KEY_OWN_COUNTER_CORRECTION = "ownCounterCorrection";
    constexpr auto KEY_REPLY_CAPITAL_LOSS = "replyCapitalLoss";
    constexpr auto KEY_REPLY_COUNTER_CAPITAL = "replyCounterCapital";
    constexpr auto KEY_NEXT_SHOT_AFTER_REPLY = "nextShotAfterReply";
    constexpr auto KEY_EXACT_NON_STOCK = "exactNonStock";
    using CaptureDangerRecord::KEY_OWNED;
    constexpr auto KEY_BASELINE = "baseline";
    using CaptureDangerRecord::KEY_PRESENT;
    using CaptureDangerRecord::KEY_FLOOR;
    using CaptureDangerRecord::KEY_CEILING;
    constexpr auto KEY_LOWER = "lower";
    constexpr auto KEY_UPPER = "upper";
    constexpr auto KEY_PRICE_COUNT = "priceCount";
    constexpr auto KEY_PRICES = "prices";
    constexpr auto KEY_SWITCHES = "switches";
    constexpr auto KEY_FROM = "from";
    constexpr auto KEY_TO = "to";
    constexpr auto KEY_TIER = "tier";
    constexpr auto KEY_INCUMBENT_UPPER = "incumbentUpper";
    constexpr auto KEY_CHANGED_LOWER = "changedLower";
    constexpr auto KEY_MARGIN = "margin";
    constexpr auto KEY_CHANGES = "changes";
    constexpr auto KEY_ROUND = "round";
    using CaptureDangerRecord::KEY_REPLY_LOSS;
    constexpr auto KEY_OUTCOME = "outcome";
    constexpr auto KEY_STOCK_STATUS = "stockStatus";
    constexpr auto KEY_VERIFICATION = "verification";
    constexpr auto KEY_PASSED = "passed";
    constexpr auto KEY_MISMATCH = "mismatch";
    constexpr auto KEY_INDEX = "index";
    constexpr auto KEY_FINAL_CHOICES = "finalChoices";
    constexpr auto KEY_FINAL_DIFFERS = "finalDiffers";

    template <typename Names, typename Value>
    QLatin1String nameOf(const Names & names, Value value)
    {
        return QLatin1String(names[static_cast<std::size_t>(value)]);
    }

    template <typename Number>
    QJsonArray numbers(std::span<const Number> values)
    {
        QJsonArray result;
        for (const Number value : values)
        {
            result.append(static_cast<qint64>(value));
        }
        return result;
    }

    QJsonValue optionalAmount(const std::optional<MilliFunds> & amount)
    {
        return amount ? QJsonValue(static_cast<qint64>(*amount)) : QJsonValue();
    }

    QJsonValue shotUnitsRecord(const std::optional<ReplyPassShotUnits> & units)
    {
        if (!units)
        {
            return QJsonValue();
        }
        return QJsonObject{
            {KEY_SHOT, static_cast<qint64>(units->shot)},
            {KEY_CHANGEABLE, static_cast<qint64>(units->changeable)},
            {KEY_NOT_CHANGEABLE, static_cast<qint64>(units->fixedRole + units->committed)},
            {KEY_FIXED_ROLE, static_cast<qint64>(units->fixedRole)},
            {KEY_COMMITTED, static_cast<qint64>(units->committed)},
        };
    }

    QJsonObject countersRecord(const ReplyPassCounters & counters)
    {
        QJsonObject noValue;
        for (std::size_t reason = 0; reason < counters.noValue.size(); ++reason)
        {
            noValue.insert(QLatin1String(REPLY_PASS_NO_VALUE_NAMES[reason]),
                           static_cast<qint64>(counters.noValue[reason]));
        }
        return QJsonObject{
            {KEY_UNITS_SHOT, static_cast<qint64>(counters.unitsShot)},
            {KEY_SHOT_CHANGEABLE, static_cast<qint64>(counters.shotChangeable)},
            {KEY_SHOT_NOT_CHANGEABLE, static_cast<qint64>(counters.shotFixedRole + counters.shotCommitted)},
            {KEY_SHOT_FIXED_ROLE, static_cast<qint64>(counters.shotFixedRole)},
            {KEY_SHOT_COMMITTED, static_cast<qint64>(counters.shotCommitted)},
            {KEY_LISTED_SHOT, static_cast<qint64>(counters.listedShot)},
            {KEY_LISTED_HITS_SHOOTER, static_cast<qint64>(counters.listedHitsShooter)},
            {KEY_NO_VALUE, noValue},
            {KEY_NO_INTERVAL, static_cast<qint64>(counters.noInterval)},
            {KEY_PROVEN_NOT_BETTER, static_cast<qint64>(counters.provenNotBetter)},
            {KEY_CANNOT_CLEAR, static_cast<qint64>(counters.cannotClear)},
            {KEY_FLOORS_BUILT, static_cast<qint64>(counters.floorsBuilt)},
            {KEY_UNDECIDED, static_cast<qint64>(counters.undecided)},
            {KEY_RETUNES, static_cast<qint64>(counters.retunes)},
            {KEY_SWITCHES_AT_FLOOR, static_cast<qint64>(counters.switchesAtFloor)},
            {KEY_SWITCHES_AT_RETUNE, static_cast<qint64>(counters.switchesAtRetune)},
            {KEY_ENDED_BY_LIMIT, counters.endedByLimit},
        };
    }

    QJsonObject costsRecord(const ReplyPassResult & result)
    {
        QJsonObject costs;
        for (std::size_t stage = 0; stage < result.costs.size(); ++stage)
        {
            const ReplyPassCost & cost = result.costs[stage];
            costs.insert(QLatin1String(REPLY_PASS_STAGE_NAMES[stage]), QJsonObject{
                {KEY_NANOS, static_cast<qint64>(cost.nanos)},
                {KEY_WORK, static_cast<qint64>(cost.work)},
                {KEY_CALLS, static_cast<qint64>(cost.calls)},
            });
        }
        return costs;
    }

    QJsonObject evidenceRecord(const ReplyPassStockEvidence & evidence)
    {
        return QJsonObject{
            {KEY_PRESENT, evidence.present},
            {KEY_FLOOR, static_cast<qint64>(evidence.floor)},
            {KEY_CEILING, static_cast<qint64>(evidence.ceiling)},
            {KEY_LOWER, static_cast<qint64>(evidence.interval.lower)},
            {KEY_UPPER, static_cast<qint64>(evidence.interval.upper)},
            {KEY_PRICE_COUNT, static_cast<qint64>(evidence.prices.size())},
            {KEY_PRICES, numbers<MilliFunds>(evidence.prices)},
        };
    }

    QJsonObject actorRecord(const ReplyPassActor & actor)
    {
        const ReplyPassTerms & terms = actor.terms;
        return QJsonObject{
            {KEY_ACTOR, actor.actorSlot},
            {KEY_UNIT, actor.engineUnitId},
            {KEY_ACTION, actor.actionIndex},
            {KEY_CANDIDATE, actor.candidateIndex},
            {KEY_GRANTED_DAMAGE, actor.grantedDamage},
            {KEY_DAY_START_STEPS, actor.dayStartSteps},
            {KEY_STEPS_AFTER_OWN_ACTIONS, actor.stepsAfterOwnActions},
            {KEY_STEPS_AFTER_REPLY, actor.stepsAfterReply},
            {KEY_DESTROYED_BY, nameOf(CaptureDangerRecord::STOCK_DEATH_CAUSE_NAMES, actor.destroyedBy)},
            {KEY_STOCK_ROW, actor.stockRow},
            {KEY_ACTING_SEATED, static_cast<qint64>(terms.actingSeated)},
            {KEY_OWN_COUNTER_CORRECTION, static_cast<qint64>(terms.ownCounterCorrection)},
            {KEY_REPLY_CAPITAL_LOSS, static_cast<qint64>(terms.replyCapitalLoss)},
            {KEY_REPLY_COUNTER_CAPITAL, static_cast<qint64>(terms.replyCounterCapital)},
            {KEY_NEXT_SHOT_AFTER_REPLY, static_cast<qint64>(terms.nextShotAfterReply)},
        };
    }

    QJsonObject planRecord(const ReplyPassPlan & plan, const std::optional<ReplyPassShotUnits> & shots)
    {
        QJsonArray actors;
        for (const ReplyPassActor & actor : plan.actors)
        {
            actors.append(actorRecord(actor));
        }
        return QJsonObject{
            {KEY_CHOICES, numbers<std::int32_t>(plan.choices)},
            {KEY_EXECUTION_ORDER, numbers<std::int32_t>(plan.executionOrder)},
            {KEY_AFTER_OWN_ACTIONS, CaptureDangerRecord::continuationKeyRecord(plan.afterOwnActions)},
            {KEY_AFTER_REPLY, CaptureDangerRecord::continuationKeyRecord(plan.afterReply)},
            {KEY_ACTORS, actors},
            {KEY_EXACT_NON_STOCK, static_cast<qint64>(plan.exactNonStock)},
            {KEY_OWNED, static_cast<qint64>(plan.owned)},
            {KEY_SHOT_UNITS, shotUnitsRecord(shots)},
            {KEY_BASELINE, evidenceRecord(plan.baseline)},
        };
    }

    QJsonObject switchRecord(const ReplyPassSwitch & made)
    {
        return QJsonObject{
            {KEY_FROM, static_cast<qint64>(made.from)},
            {KEY_TO, static_cast<qint64>(made.to)},
            {KEY_ACTOR, made.actorSlot},
            {KEY_CANDIDATE, made.candidateIndex},
            {KEY_REASON, nameOf(CHANGE_REASON_NAMES, made.reason)},
            {KEY_TIER, nameOf(REPLY_PASS_TIER_NAMES, made.tier)},
            {KEY_INCUMBENT_UPPER, static_cast<qint64>(made.incumbentUpper)},
            {KEY_CHANGED_LOWER, static_cast<qint64>(made.changedLower)},
            {KEY_MARGIN, static_cast<qint64>(made.margin)},
        };
    }

    QJsonObject changeRecord(const ReplyPassChange & change)
    {
        return QJsonObject{
            {KEY_ROUND, change.round},
            {KEY_ACTOR, change.actorSlot},
            {KEY_UNIT, change.engineUnitId},
            {KEY_CANDIDATE, change.candidateIndex},
            {KEY_REASON, nameOf(CHANGE_REASON_NAMES, change.reason)},
            {KEY_REPLY_LOSS, static_cast<qint64>(change.replyLoss)},
            {KEY_TIER, nameOf(REPLY_PASS_TIER_NAMES, change.tier)},
            {KEY_OUTCOME, nameOf(REPLY_PASS_OUTCOME_NAMES, change.outcome)},
            {KEY_NO_VALUE, nameOf(REPLY_PASS_NO_VALUE_NAMES, change.noValue)},
            {KEY_STOCK_STATUS, nameOf(REPLY_STOCK_STATUS_NAMES, change.stockStatus)},
            {KEY_LOWER, optionalAmount(change.lower)},
            {KEY_UPPER, optionalAmount(change.upper)},
        };
    }

    QJsonObject verificationRecord(const ReplyPassRun & run)
    {
        const auto & verification = run.verification;
        return QJsonObject{
            {KEY_RAN, verification.has_value()},
            {KEY_PASSED, verification && verification->verified()},
            {KEY_MISMATCH, verification ? QJsonValue(nameOf(REPLY_PASS_MISMATCH_NAMES, verification->mismatch))
                                        : QJsonValue()},
            {KEY_INDEX, verification ? QJsonValue(static_cast<qint64>(verification->index)) : QJsonValue()},
        };
    }

    void insertResult(QJsonObject & record, const ReplyPassRun & run, const ReplyPassResult & result)
    {
        QJsonArray plans;
        for (std::size_t index = 0; index < result.plans.size(); ++index)
        {
            const bool measured = index < run.acceptedPlanShots.size();
            plans.append(planRecord(result.plans[index], measured ? run.acceptedPlanShots[index] : std::nullopt));
        }
        QJsonArray switches;
        for (const ReplyPassSwitch & made : result.switches)
        {
            switches.append(switchRecord(made));
        }
        QJsonArray changes;
        for (const ReplyPassChange & change : result.changes)
        {
            changes.append(changeRecord(change));
        }
        record.insert(KEY_END, nameOf(REPLY_PASS_END_NAMES, result.end));
        record.insert(KEY_INCUMBENT_NO_VALUE, nameOf(REPLY_PASS_NO_VALUE_NAMES, result.incumbentNoValue));
        record.insert(KEY_BASELINE_STATUS, nameOf(REPLY_STOCK_STATUS_NAMES, result.baselineStatus));
        record.insert(KEY_STOCK_CONTEXT_BUILT, result.stockContextBuilt);
        record.insert(KEY_SHOT_UNITS, shotUnitsRecord(run.planInEffectShots));
        record.insert(KEY_COUNTERS, countersRecord(result.counters));
        record.insert(KEY_COSTS, costsRecord(result));
        record.insert(KEY_PLANS, plans);
        record.insert(KEY_SWITCHES, switches);
        record.insert(KEY_CHANGES, changes);
        record.insert(KEY_FINAL_CHOICES, numbers<std::int32_t>(result.finalChoices));
        record.insert(KEY_FINAL_DIFFERS, result.finalChoices != run.planChoices);
    }

    std::optional<ReplyPassShotUnits> shotUnitsOf(const MasterAssignmentProblem & problem,
                                                  std::span<const std::int32_t> choices, const ReplyBoard & board)
    {
        const ReplyPassEvaluation evaluation = evaluateReplyPassPlan(problem, choices, board);
        if (!evaluation.valued())
        {
            return std::nullopt;
        }
        ReplyPassCounters counters;
        static_cast<void>(ReplyPassDetail::listChanges(problem, choices, evaluation, 0, counters));
        return ReplyPassShotUnits{counters.unitsShot, counters.shotChangeable, counters.shotFixedRole,
                                  counters.shotCommitted};
    }

    ReplyPassSkip skipOf(bool localSession, bool legacyPlanning, const MasterPlanHandoffResult & handoff,
                         const ReplyBoard* pBoard)
    {
        if (!localSession)
        {
            return ReplyPassSkip::NotLocalSession;
        }
        if (pBoard == nullptr)
        {
            return ReplyPassSkip::NoReplyBoard;
        }
        if (legacyPlanning)
        {
            return ReplyPassSkip::LegacyPlan;
        }
        return handoff.handoff ? ReplyPassSkip::None : ReplyPassSkip::NoHandoff;
    }
}

namespace Coordinator
{
    ReplyPassWiring replyPassWiring(ReplyPrediction prediction, ReplyPassMode mode, bool localSession)
    {
        const bool pass = mode == ReplyPassMode::Recorded;
        return ReplyPassWiring{
            .replyFacts = localSession && (pass || prediction == ReplyPrediction::Recorded),
            .handoff = localSession && pass,
            .record = pass,
        };
    }

    ReplyPassRun recordedReplyPass(bool localSession, bool legacyPlanning, const MasterPlanHandoffResult & handoff,
                                   const ReplyBoard* pBoard, const PropertyStockField & field)
    {
        ReplyPassRun run;
        run.handoff = handoff.status;
        run.skip = skipOf(localSession, legacyPlanning, handoff, pBoard);
        if (run.skip != ReplyPassSkip::None)
        {
            return run;
        }
        const MasterPlanHandoff & plan = *handoff.handoff;
        run.planChoices = plan.choices;
        const auto passStart = std::chrono::steady_clock::now();
        run.result = runReplyPass(plan.problem, plan.choices, *pBoard, field);
        run.passNanos = nanosSince(passStart);
        const auto shotStart = std::chrono::steady_clock::now();
        run.planInEffectShots = shotUnitsOf(plan.problem, plan.choices, *pBoard);
        for (const ReplyPassPlan & accepted : run.result->plans)
        {
            run.acceptedPlanShots.push_back(shotUnitsOf(plan.problem, accepted.choices, *pBoard));
        }
        run.shotNanos = nanosSince(shotStart);
        if (!run.result->plans.empty())
        {
            const auto verificationStart = std::chrono::steady_clock::now();
            run.verification = verifyReplyPass(plan.problem, *pBoard, field, *run.result);
            run.verificationNanos = nanosSince(verificationStart);
        }
        return run;
    }

    QJsonObject replyPassRecord(const DecisionTraceIdentity & identity, const ReplyPassRun & run)
    {
        QJsonObject record{
            {KEY_SCHEMA, REPLY_PASS_RECORD_SCHEMA},
            {KEY_DAY, identity.day},
            {KEY_PLAYER, identity.playerId},
            {KEY_PLAN_SEQUENCE, static_cast<qint64>(identity.planSequence)},
            {KEY_MODE, nameOf(REPLY_PASS_MODE_NAMES, run.mode)},
            {KEY_RAN, run.result.has_value()},
            {KEY_REASON, nameOf(REPLY_PASS_SKIP_NAMES, run.skip)},
            {KEY_HANDOFF, nameOf(MASTER_PLAN_HANDOFF_STATUS_NAMES, run.handoff)},
            {KEY_PASS_NANOS, static_cast<qint64>(run.passNanos)},
            {KEY_SHOT_NANOS, static_cast<qint64>(run.shotNanos)},
            {KEY_VERIFICATION_NANOS, static_cast<qint64>(run.verificationNanos)},
            {KEY_PLAN_CHOICES, numbers<std::int32_t>(run.planChoices)},
            {KEY_VERIFICATION, verificationRecord(run)},
        };
        if (run.result)
        {
            insertResult(record, run, *run.result);
        }
        return record;
    }
}
