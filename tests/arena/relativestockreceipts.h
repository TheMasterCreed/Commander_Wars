#pragma once

#include <cmath>

#include <QJsonArray>
#include <QVariantMap>

#include "ai/coordinator/relativestockcomparison.h"

namespace ArenaStockReceipts
{
    inline bool readTuple(const QJsonValue & value, std::vector<std::int32_t> & tuple)
    {
        constexpr qsizetype MAX_TUPLE_ACTORS = 64;
        constexpr std::int32_t MAX_CANDIDATE_INDEX = 100000;
        if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > MAX_TUPLE_ACTORS)
        {
            return false;
        }
        for (const auto & candidate : value.toArray())
        {
            const auto number = candidate.toDouble(-1);
            if (!candidate.isDouble() || number < 0 || number > MAX_CANDIDATE_INDEX || number != std::floor(number))
            {
                return false;
            }
            tuple.push_back(static_cast<std::int32_t>(number));
        }
        return true;
    }

    inline QVariantList indices(std::span<const std::int32_t> values)
    {
        QVariantList result;
        for (const auto value : values)
        {
            result.push_back(value);
        }
        return result;
    }

    inline QVariantMap quoteReceipt(const Coordinator::RelativeStockQuote & quote)
    {
        QVariantList key;
        for (const auto & action : quote.key)
        {
            key.push_back(QVariantMap{{"row", action.row()}, {"x", action.x()}, {"y", action.y()},
                                     {"captures", action.captures()}});
        }
        return {
            {"key", key}, {"capturedColumns", indices(quote.capturedColumns)},
            {"economic", static_cast<qint64>(quote.economic)}, {"owned", static_cast<qint64>(quote.owned)},
            {"ourLower", static_cast<qint64>(quote.ours.lower)}, {"ourUpper", static_cast<qint64>(quote.ours.upper)},
            {"enemyLower", static_cast<qint64>(quote.enemy.lower)}, {"enemyUpper", static_cast<qint64>(quote.enemy.upper)},
            {"ourLowerWitnessReplays", quote.ourLowerWitnessReplays},
            {"enemyLowerWitnessReplays", quote.enemyLowerWitnessReplays},
            {"ourSearchCompleted", quote.ourSearchCompleted}, {"enemySearchCompleted", quote.enemySearchCompleted},
            {"ourUpperCertificate", static_cast<qulonglong>(quote.ourUpperCertificateId)},
            {"absoluteExact", quote.absoluteExact()}, {"identityLive", quote.identity.established()}, {"valid", quote.valid},
        };
    }

    inline QVariantMap comparisonReceipt(const Coordinator::RelativeStockComparison & comparison)
    {
        return {
            {"deltaLower", static_cast<qint64>(comparison.deltaLower)},
            {"deltaUpper", static_cast<qint64>(comparison.deltaUpper)},
            {"economicDelta", static_cast<qint64>(comparison.economicDelta)},
            {"ownedDelta", static_cast<qint64>(comparison.ownedDelta)},
            {"commonEnemyCancelled", comparison.commonEnemyCancelled}, {"valid", comparison.valid()},
            {"relativeExact", comparison.exact()}, {"improves", comparison.improves()},
            {"unresolved", comparison.unresolved()}, {"reason", static_cast<int>(comparison.reason)},
        };
    }
}
