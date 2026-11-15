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
#include "ai/coordinator/ownactionmeasurement.h"
#include "ai/coordinator/replycheck.h"
#include "ai/coordinator/stockreplykeys.h"

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

    using Coordinator::StockQuoteStatus;

    inline constexpr std::array STOCK_QUOTE_STATUS_NAMES{
        "AVAILABLE", "MISSING_STOCK_FIELD", "UNUSABLE_PLAN_CHAIN", "REPLAY_FAILED",
        "CYCLIC_ORDERING", "MISSING_ACTION_FACTS",
        "UNSUPPORTED_ACTION", "TARGET_ALREADY_GONE",
    };

    inline const char* stockQuoteStatusName(StockQuoteStatus status)
    {
        return STOCK_QUOTE_STATUS_NAMES[static_cast<std::size_t>(status)];
    }

    using Coordinator::StockDeathCause;
    using Coordinator::StockReplyKeys;
    using Coordinator::stockReplyKeys;

    inline constexpr std::array STOCK_DEATH_CAUSE_NAMES{
        INTERRUPTION_NONE, "OWN_ACTION", "REPLY",
    };

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

    struct StockPlanMeasurement : OwnActionMeasurement
    {
        std::vector<std::int32_t> planSlots;
        PlanReplyRun reply;
    };

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
            const bool startKnown = replyStartKnown(entries[slot].pBundle != nullptr, plan.action(slot).kind);
            const auto facts = ownActionFacts(entries[slot], replies[slot].target.hp, startKnown);
            if (facts != StockQuoteStatus::Available)
            {
                result.status = facts;
                return result;
            }
            ordered.push_back(entries[slot]);
            targets.push_back(replies[slot]);
        }
        static_cast<OwnActionMeasurement &>(result) = measureOwnActions(ordered, targets);
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
