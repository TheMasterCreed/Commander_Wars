#pragma once

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
            case ExactStatus::NotOnProperty:
                break;
        }
        return STATUS_NOT_ON_PROPERTY;
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
