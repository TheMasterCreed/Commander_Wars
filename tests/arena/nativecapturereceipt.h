#pragma once

#include <optional>

#include <QPoint>
#include <QString>
#include <QVariantMap>

namespace NativeCaptureReceipt
{
    inline const QString FIELD = QStringLiteral("nativeCapture");
    inline const QString BOUNDARY = QStringLiteral("ActionPerformer.sigActionPerformed");
    constexpr qint32 VERSION = 1;

    struct State
    {
        QString propertyId;
        qint32 propertyOwner{-1};
        qint32 actorOwner{-1};
        qint32 progress{0};
    };

    struct Observation
    {
        qint32 actorId{-1};
        QPoint target;
        State before;
        std::optional<State> after;
        bool actorOnTarget{false};
        bool actionConsumed{false};
    };

    inline QVariantMap stateRecord(const State & state)
    {
        return {{"propertyId", state.propertyId}, {"propertyOwner", state.propertyOwner},
            {"actorOwner", state.actorOwner}, {"progress", state.progress}};
    }

    inline QVariantMap record(const Observation & observation)
    {
        QVariantMap result{{"version", VERSION}, {"boundary", BOUNDARY}, {"actorId", observation.actorId},
            {"target", QVariantMap{{"x", observation.target.x()}, {"y", observation.target.y()}}},
            {"before", stateRecord(observation.before)}, {"nativeObjectsLive", observation.after.has_value()}};
        if (observation.after)
        {
            result.insert("after", stateRecord(*observation.after));
            result.insert("actorOnTarget", observation.actorOnTarget);
            result.insert("actionConsumed", observation.actionConsumed);
        }
        else
        {
            result.insert("after", QVariant{});
            result.insert("actorOnTarget", QVariant{});
            result.insert("actionConsumed", QVariant{});
        }
        return result;
    }
}
