#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <QString>

#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/turnplan.h"

namespace Coordinator
{
    struct DecisionTraceIdentity
    {
        std::int32_t day{0};
        std::int32_t playerId{NO_OWNER};
        std::uint64_t planSequence{0};
        std::int32_t horizonTurns{0};
    };

    class DecisionTrace
    {
    public:
        virtual ~DecisionTrace() = default;

        virtual bool candidateDetailsEnabled() const = 0;
        virtual bool stockDetailsEnabled() const = 0;
        virtual void record(const QString & category, const QString & fields) = 0;
        virtual bool flush() = 0;
    };

    bool decisionTraceEnabled();
    bool planningTimingAuditEnabled();
    std::unique_ptr<DecisionTrace> openDecisionTrace(const DecisionTraceIdentity & identity);
    void writePlanningTimingAudit(
        const DecisionTraceIdentity & identity,
        const QString & fields);

    inline QString traceBool(bool value)
    {
        return value ? QStringLiteral("true") : QStringLiteral("false");
    }

    inline QString traceTile(const TilePoint & tile)
    {
        return QStringLiteral("(%1,%2)").arg(tile.x).arg(tile.y);
    }

    inline QString tracePath(std::span<const TilePoint> path)
    {
        QString text;
        for (std::size_t slot = 0; slot < path.size(); ++slot)
        {
            if (slot > 0)
            {
                text += QLatin1Char('>');
            }
            text += traceTile(path[slot]);
        }
        return text;
    }

    inline QString traceIndices(std::span<const std::int32_t> indices)
    {
        QString text;
        for (std::size_t slot = 0; slot < indices.size(); ++slot)
        {
            if (slot > 0)
            {
                text += QLatin1Char(',');
            }
            text += QString::number(indices[slot]);
        }
        return text;
    }

    inline QString traceBundleKind(PlanBundleKind kind)
    {
        switch (kind)
        {
            case PlanBundleKind::Wait:
                return QStringLiteral("WAIT");
            case PlanBundleKind::Move:
                return QStringLiteral("POSITIONAL");
            case PlanBundleKind::Fire:
                return QStringLiteral("FIRE");
            case PlanBundleKind::MoveAndFire:
                return QStringLiteral("MOVE_AND_FIRE");
            case PlanBundleKind::Capture:
                return QStringLiteral("CAPTURE");
            case PlanBundleKind::MoveAndCapture:
                return QStringLiteral("MOVE_AND_CAPTURE");
            case PlanBundleKind::Service:
                return QStringLiteral("SERVICE");
            case PlanBundleKind::MoveAndService:
                return QStringLiteral("MOVE_AND_SERVICE");
            case PlanBundleKind::Compound:
                return QStringLiteral("COMPOUND");
        }
        return QStringLiteral("UNKNOWN");
    }

    inline QString traceReservationResult(ReservationResult result)
    {
        switch (result)
        {
            case ReservationResult::Granted:
                return QStringLiteral("GRANTED");
            case ReservationResult::Conflict:
                return QStringLiteral("CONFLICT");
            case ReservationResult::Overkill:
                return QStringLiteral("OVERKILL");
            case ReservationResult::StaleTarget:
                return QStringLiteral("STALE_TARGET");
            case ReservationResult::Invalid:
                return QStringLiteral("INVALID");
        }
        return QStringLiteral("UNKNOWN");
    }

    inline QString tracePlanActionState(PlanActionState state)
    {
        switch (state)
        {
            case PlanActionState::Pending:
                return QStringLiteral("PENDING");
            case PlanActionState::Blocked:
                return QStringLiteral("BLOCKED");
            case PlanActionState::Committed:
                return QStringLiteral("COMMITTED");
            case PlanActionState::Abandoned:
                return QStringLiteral("ABANDONED");
        }
        return QStringLiteral("UNKNOWN");
    }
}
