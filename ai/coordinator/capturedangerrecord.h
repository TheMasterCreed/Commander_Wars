#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>

#include "ai/coordinator/capturedanger.h"
#include "ai/coordinator/continuationinterval.h"
#include "ai/coordinator/capturedangerstream.h"
#include "ai/coordinator/replycheck.h"

namespace Coordinator::CaptureDangerRecord
{
    constexpr auto KEY_HP = "hp";
    constexpr auto KEY_DAMAGE = "damage";
    constexpr auto KEY_APPLIED = "applied";
    constexpr auto KEY_DELIVERED = "delivered";
    constexpr auto KEY_UNREPLIED = "unreplied";
    constexpr auto KEY_DESTROYED = "destroyed";
    constexpr auto KEY_HP_END = "hpEnd";
    constexpr auto KEY_HP_REAL_END = "hpRealEnd";
    constexpr auto KEY_POINTS_END = "pointsEnd";
    constexpr auto KEY_REPLIES = "replies";
    constexpr auto KEY_STREAM = "stream";
    constexpr auto STATUS_COMPUTED = "COMPUTED";
    constexpr auto STATUS_NO_SHOOTER = "NO_SHOOTER";
    constexpr auto STATUS_NO_WEAPON = "NO_WEAPON";
    constexpr auto STATUS_ENGINE_REFUSED = "ENGINE_REFUSED";
    constexpr auto STATUS_NO_FIRING_TILE = "NO_FIRING_TILE";
    constexpr auto STATUS_NOT_ON_PROPERTY = "NOT_ON_PROPERTY";
    constexpr auto STATUS_AMMO_GATED = "AMMO_GATED";
    constexpr auto STATUS_REPAIR_PENDING = "REPAIR_PENDING";
    constexpr auto STATUS_NO_LIVE_SHOOTER = "NO_LIVE_SHOOTER";
    constexpr auto STATUS_NOT_QUERIED = "NOT_QUERIED";
    constexpr auto KEY_HP_AFTER_REPLY = "hpAfterReply";
    constexpr auto KEY_HP_REAL_AFTER_REPLY = "hpRealAfterReply";
    constexpr auto KEY_SHOTS_APPLIED = "shotsApplied";
    constexpr auto KEY_FIRES = "fires";
    constexpr auto KEY_WORTH = "worth";
    constexpr auto KEY_COUNTER_COST = "counterCost";
    constexpr auto KEY_CAPTURE_COST = "captureCost";
    constexpr auto KEY_PRICED_SHOTS = "shots";
    constexpr auto RESOLUTION_KNOWN = "KNOWN";
    constexpr auto RESOLUTION_AMMO_RESUPPLIED = "AMMO_RESUPPLIED";
    constexpr auto RESOLUTION_AMMO_AWAY = "AMMO_AWAY";
    constexpr auto RESOLUTION_REPAIRED = "REPAIRED";
    constexpr auto INTERRUPTION_NONE = "NONE";
    constexpr auto INTERRUPTION_DELAYED = "DELAYED";
    constexpr auto INTERRUPTION_RESTARTED = "RESTARTED";

    enum class StockQuoteStatus : std::int8_t
    {
        Available,
        MissingField,
        UnusablePlanChain,
        ReplayFailed,
        CyclicOrdering,
        MissingActionFacts,
        UnsupportedAction,
        TargetAlreadyGone,
    };

    inline constexpr std::array STOCK_QUOTE_STATUS_NAMES{
        "AVAILABLE", "MISSING_STOCK_FIELD", "UNUSABLE_PLAN_CHAIN", "REPLAY_FAILED",
        "CYCLIC_ORDERING", "MISSING_ACTION_FACTS",
        "UNSUPPORTED_ACTION", "TARGET_ALREADY_GONE",
    };

    inline const char* stockQuoteStatusName(StockQuoteStatus status)
    {
        return STOCK_QUOTE_STATUS_NAMES[static_cast<std::size_t>(status)];
    }

    enum class StockDeathCause : std::int8_t
    {
        None,
        OwnAction,
        Reply,
    };

    inline constexpr std::array STOCK_DEATH_CAUSE_NAMES{
        INTERRUPTION_NONE, "OWN_ACTION", "REPLY",
    };

    struct StockReplyKeys
    {
        CanonicalPlanActionKey afterOwnActions;
        CanonicalPlanActionKey afterReply;
        StockDeathCause death{StockDeathCause::None};
    };

    inline std::optional<StockReplyKeys> stockReplyKeys(
        const CanonicalPlanActionKey & today, std::int32_t dayStartSteps,
        std::int32_t counterSteps, std::int32_t replySteps)
    {
        if (today.stepsLost() != 0 || today.destroyed() || dayStartSteps <= 0 ||
            dayStartSteps > CanonicalPlanActionKey::MAX_STEPS_LOST || counterSteps < 0 || replySteps < 0 ||
            counterSteps > dayStartSteps || replySteps > dayStartSteps - counterSteps)
        {
            return std::nullopt;
        }
        const bool diedAttacking = counterSteps == dayStartSteps;
        const bool dead = counterSteps + replySteps == dayStartSteps;
        return StockReplyKeys{
            *CanonicalPlanActionKey::checkedFromAction(today.row(), today.x(), today.y(), today.captures(),
                                                       counterSteps, diedAttacking),
            *CanonicalPlanActionKey::checkedFromAction(today.row(), today.x(), today.y(), today.captures(),
                                                       counterSteps + replySteps, dead),
            diedAttacking ? StockDeathCause::OwnAction : dead ? StockDeathCause::Reply : StockDeathCause::None,
        };
    }

    constexpr StockInterval stockQuoteLoss(const StockInterval & before, const StockInterval & after)
    {
        return {before.lower - after.upper, before.upper - after.lower};
    }

    constexpr StockQuoteStatus stockQuoteStatus(bool fieldAvailable, bool chainUsable, bool replayValid)
    {
        return !fieldAvailable ? StockQuoteStatus::MissingField :
               !chainUsable ? StockQuoteStatus::UnusablePlanChain :
               !replayValid ? StockQuoteStatus::ReplayFailed : StockQuoteStatus::Available;
    }

    struct MeasuredCounter
    {
        StockQuoteStatus status{StockQuoteStatus::Available};
        std::int32_t isolatedSteps{0};
        std::int32_t steps{0};
        bool counterScaled{false};
        double hpAfter{0};
    };

    inline MeasuredCounter measuredCounter(const FireFacts & fire, std::int32_t targetHp,
                                           std::int32_t actorHp, double actorRealHp)
    {
        MeasuredCounter result{.isolatedSteps = fire.counterSteps, .hpAfter = actorRealHp};
        if (targetHp <= 0)
        {
            return result;
        }
        const auto isolatedHp = fire.targetHpSteps - std::min(fire.damageSteps, fire.targetHpSteps);
        if (isolatedHp <= 0 || targetHp > isolatedHp)
        {
            result.status = StockQuoteStatus::UnusablePlanChain;
            return result;
        }
        result.steps = std::min(fire.counterSteps, actorHp);
        if (targetHp < isolatedHp)
        {
            // FireFacts retains steps only; use the oracle's real-hp loss conversion after scaling.
            const double damage = static_cast<double>(fire.counterSteps) * targetHp * FastBattleEstimate::HP_SCALE / isolatedHp;
            result.steps = std::min(damageStepsFrom(actorRealHp, damage), actorHp);
            result.counterScaled = true;
        }
        result.hpAfter = realHpAfterAction(actorRealHp, ActionHpOutcome{
            .hpSteps = actorHp - result.steps, .realHpFromSteps = result.steps > 0,
        });
        return result;
    }

    constexpr std::int32_t UNKNOWN_ACTION_HP = -1;

    struct StockPlanMeasurement
    {
        StockQuoteStatus status{StockQuoteStatus::UnusablePlanChain};
        std::vector<std::int32_t> planSlots;
        std::vector<StockQuoteStatus> ownStatus;
        PlanChainRun chain;
        std::vector<double> hpAfterActions;
        PlanReplyRun reply;
        std::vector<MeasuredCounter> counters;
        std::int32_t certainCounterUnits{0};
        std::int32_t scaledCounterUnits{0};
        std::int32_t scaledCounterSurvivalDifferences{0};
    };

    template <typename UnitAt>
    PlannedOccupancy stockReplyOccupancy(const PlannedOccupancy & live, const StockPlanMeasurement & measurement,
                                         UnitAt && unitAt)
    {
        std::vector<std::int32_t> dead;
        for (std::size_t index = 0; index < measurement.planSlots.size(); ++index)
        {
            if (measurement.chain.outcomes[index].hpSteps <= 0)
            {
                dead.push_back(unitAt(measurement.planSlots[index]));
            }
        }
        return live.withoutUnits(dead);
    }

    template <typename PrepareReply, typename StartHp, typename DamageAt>
    StockPlanMeasurement measureStockPlan(const TurnPlan & plan, std::span<const PlanChainEntry> entries,
                                         std::span<const ReplyTargetFacts> replies, PrepareReply && prepareReply,
                                         StartHp && startHp, DamageAt && damageAt)
    {
        StockPlanMeasurement result;
        const auto ordering = plan.executionOrder();
        if (!orderingAcyclic(ordering))
        {
            result.status = StockQuoteStatus::CyclicOrdering;
            return result;
        }
        if (entries.size() != static_cast<std::size_t>(plan.actionCount()) || replies.size() != entries.size())
        {
            result.status = StockQuoteStatus::MissingActionFacts;
            return result;
        }
        std::vector<PlanChainEntry> ordered;
        std::vector<ReplyTargetFacts> targets;
        for (const auto slot : ordering.order)
        {
            if (!isLiveState(plan.action(slot).state))
            {
                continue;
            }
            result.planSlots.push_back(slot);
            if (entries[slot].actorHpSteps == UNKNOWN_ACTION_HP ||
                entries[slot].actorHpSteps != ceilHpSteps(replies[slot].target.hp) ||
                !replyStartKnown(entries[slot].pBundle != nullptr, plan.action(slot).kind))
            {
                result.status = StockQuoteStatus::MissingActionFacts;
                return result;
            }
            if (entries[slot].pBundle != nullptr &&
                (entries[slot].pBundle->components.size() > 1 || servesOtherUnit(*entries[slot].pBundle)))
            {
                result.status = StockQuoteStatus::UnsupportedAction;
                return result;
            }
            ordered.push_back(entries[slot]);
            targets.push_back(replies[slot]);
        }
        result.ownStatus.assign(ordered.size(), StockQuoteStatus::Available);
        std::vector<bool> hasCounterResult(ordered.size(), false);
        result.counters.resize(ordered.size());
        result.chain = planChain(ordered, [&](std::size_t index, const FireFacts & fire,
                                              std::int32_t targetHp, std::int32_t actorHp)
        {
            const auto counter = measuredCounter(fire, targetHp, actorHp, targets[index].target.hp);
            result.counters[index] = counter;
            result.ownStatus[index] = counter.status;
            hasCounterResult[index] = true;
            if (counter.status != StockQuoteStatus::Available)
            {
                return ActionCounterDamage{};
            }
            targets[index].target.hp = counter.hpAfter;
            return ActionCounterDamage{counter.steps, ActionCounterSource::Repriced};
        });
        result.status = StockQuoteStatus::Available;
        result.hpAfterActions.resize(ordered.size());
        for (std::size_t index = 0; index < ordered.size(); ++index)
        {
            if (!result.chain.targetsAlive[index])
            {
                result.ownStatus[index] = StockQuoteStatus::TargetAlreadyGone;
            }
            if (result.ownStatus[index] != StockQuoteStatus::Available)
            {
                if (result.status == StockQuoteStatus::Available)
                {
                    result.status = result.ownStatus[index];
                }
                continue;
            }
            if (!hasCounterResult[index])
            {
                targets[index].target.hp = realHpAfterAction(targets[index].target.hp, result.chain.outcomes[index]);
            }
            else if (result.counters[index].counterScaled)
            {
                ++result.scaledCounterUnits;
                const auto isolatedHp = ordered[index].actorHpSteps -
                    std::min(result.counters[index].isolatedSteps, ordered[index].actorHpSteps);
                result.scaledCounterSurvivalDifferences +=
                    (result.chain.outcomes[index].hpSteps <= 0) != (isolatedHp <= 0) ? 1 : 0;
            }
            else
            {
                ++result.certainCounterUnits;
            }
            result.hpAfterActions[index] = targets[index].target.hp;
        }
        if (result.status != StockQuoteStatus::Available)
        {
            return result;
        }
        prepareReply(result, std::span<ReplyTargetFacts>(targets));
        result.reply = planReply(targets, result.chain.chain, std::forward<StartHp>(startHp),
            [&](std::size_t target, std::size_t slot, std::int32_t shooterHp, std::int32_t hp)
            {
                return damageAt(result.planSlots[target], slot, shooterHp, hp);
            });
        result.status = StockQuoteStatus::Available;
        return result;
    }

    inline const char* statusName(ShotStatus status)
    {
        switch (status)
        {
            case ShotStatus::Computed:
                return STATUS_COMPUTED;
            case ShotStatus::NoWeapon:
                return STATUS_NO_WEAPON;
            case ShotStatus::EngineRefused:
                return STATUS_ENGINE_REFUSED;
            case ShotStatus::NoFiringTile:
                return STATUS_NO_FIRING_TILE;
            case ShotStatus::AmmoGated:
                return STATUS_AMMO_GATED;
            case ShotStatus::NoLiveShooter:
                return STATUS_NO_LIVE_SHOOTER;
            case ShotStatus::RepairPending:
                return STATUS_REPAIR_PENDING;
            case ShotStatus::NotQueried:
                return STATUS_NOT_QUERIED;
            case ShotStatus::NotOnProperty:
                break;
        }
        return STATUS_NOT_ON_PROPERTY;
    }

    inline const char* statusName(ExactStatus status)
    {
        switch (status)
        {
            case ExactStatus::Computed:
                return STATUS_COMPUTED;
            case ExactStatus::NoShooter:
                return STATUS_NO_SHOOTER;
            case ExactStatus::NoWeapon:
                return STATUS_NO_WEAPON;
            case ExactStatus::EngineRefused:
                return STATUS_ENGINE_REFUSED;
            case ExactStatus::NoFiringTile:
                return STATUS_NO_FIRING_TILE;
            case ExactStatus::AmmoGated:
                return STATUS_AMMO_GATED;
            case ExactStatus::NoLiveShooter:
                return STATUS_NO_LIVE_SHOOTER;
            case ExactStatus::RepairPending:
                return STATUS_REPAIR_PENDING;
            case ExactStatus::NotQueried:
                return STATUS_NOT_QUERIED;
            case ExactStatus::NotOnProperty:
                break;
        }
        return STATUS_NOT_ON_PROPERTY;
    }

    inline const char* resolutionName(PricedResolution resolution)
    {
        switch (resolution)
        {
            case PricedResolution::Known:
                return RESOLUTION_KNOWN;
            case PricedResolution::AmmoResupplied:
                return RESOLUTION_AMMO_RESUPPLIED;
            case PricedResolution::AmmoAway:
                return RESOLUTION_AMMO_AWAY;
            case PricedResolution::Repaired:
                return RESOLUTION_REPAIRED;
            case PricedResolution::NoLiveShooter:
                return STATUS_NO_LIVE_SHOOTER;
            case PricedResolution::NoFiringTile:
                break;
        }
        return STATUS_NO_FIRING_TILE;
    }

    inline const char* interruptionName(CaptureInterruption interruption)
    {
        switch (interruption)
        {
            case CaptureInterruption::None:
                return INTERRUPTION_NONE;
            case CaptureInterruption::Delayed:
                return INTERRUPTION_DELAYED;
            case CaptureInterruption::Restarted:
                break;
        }
        return INTERRUPTION_RESTARTED;
    }

    inline QJsonObject outcomeRecord(const ReplyOutcome & outcome)
    {
        return QJsonObject{
            {KEY_HP_AFTER_REPLY, outcome.hpSteps},
            {KEY_HP_REAL_AFTER_REPLY, outcome.hp},
            {KEY_DESTROYED, outcome.destroyed},
            {KEY_SHOTS_APPLIED, static_cast<qint64>(std::count(outcome.fired.begin(), outcome.fired.end(), true))},
            {KEY_DAMAGE, outcome.reply.damage},
            {KEY_APPLIED, outcome.reply.applied},
        };
    }

    // An unknown R1 or R2 reply is null; P always resolves.
    inline QJsonValue outcomeRecord(const std::optional<ReplyOutcome> & outcome)
    {
        return outcome ? QJsonValue(outcomeRecord(*outcome)) : QJsonValue();
    }

    inline QJsonObject pricedShotRecord(const PricedShot & shot)
    {
        return QJsonObject{
            {KEY_FIRES, shot.fires},
            {KEY_WORTH, static_cast<qint64>(shot.worth)},
            {KEY_COUNTER_COST, static_cast<qint64>(shot.counterCost)},
            {KEY_CAPTURE_COST, static_cast<qint64>(shot.captureCost)},
        };
    }

    inline QJsonValue shotEntry(const ShotReport & shot)
    {
        if (shot.status == ShotStatus::Computed)
        {
            return QJsonValue(shot.damage);
        }
        return QJsonValue(QLatin1String(statusName(shot.status)));
    }

    inline QJsonValue turnRecord(std::int32_t turn)
    {
        return turn == NO_DELIVERY_TURN ? QJsonValue() : QJsonValue(turn);
    }

    // A walk that could not be computed is null; the stream appears only when a stock column is priced.
    inline QJsonValue walkRecord(const std::optional<CaptureWalkResult> & walk, const PropertyStockColumn* pColumn,
                                 std::int32_t horizonTurns)
    {
        if (!walk)
        {
            return QJsonValue();
        }
        QJsonArray replies;
        for (const CaptureReply & reply : walk->replies)
        {
            replies.append(QJsonObject{{KEY_DAMAGE, reply.damage}, {KEY_APPLIED, reply.applied}, {KEY_HP, reply.hpSteps}});
        }
        QJsonObject record{
            {KEY_DELIVERED, turnRecord(walk->deliveredTurn)},
            {KEY_UNREPLIED, turnRecord(walk->unrepliedDeliveredTurn)},
            {KEY_DESTROYED, walk->destroyed},
            {KEY_HP_END, walk->hpSteps},
            {KEY_HP_REAL_END, walk->hp},
            {KEY_POINTS_END, walk->points},
            {KEY_REPLIES, replies},
        };
        if (pColumn != nullptr)
        {
            record.insert(KEY_STREAM, captureWalkStream(*pColumn, walk->deliveredTurn, horizonTurns));
        }
        return record;
    }
}
