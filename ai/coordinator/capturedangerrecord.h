#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>

#include "ai/coordinator/capturedanger.h"
#include "ai/coordinator/capturedangerstream.h"

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
