#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

#include "ai/coordinator/masterassignmentfuture.h"
#include "ai/coordinator/packingdualcertificate.h"

namespace Coordinator
{
    inline bool wellFormedPackingDualHint(std::span<const MilliFunds> prices, std::size_t columns)
    {
        return prices.size() == columns &&
            std::none_of(prices.begin(), prices.end(), [](MilliFunds price) { return price < 0; });
    }

    // Used nodes stay at zero; the verifier rejects anything else.
    inline PackingDualOptimizerResult hintedPackingDualPrices(
        const PackingDualProblemIdentity & identity, std::span<const PropertyStockColumn> columns,
        std::span<const MilliFunds> hint, std::int64_t operationLimit)
    {
        PackingDualOptimizerResult result;
        if (hint.size() != columns.size() || !std::in_range<std::int64_t>(columns.size()))
        {
            return result;
        }
        const auto lookupWork = static_cast<std::int64_t>(columns.size()) + 1;
        PackingDualWorkBudget budget(operationLimit);
        result.nodePrices.assign(identity.nodeColumns.size(), 0);
        for (std::size_t node = 0; node < identity.nodeColumns.size(); ++node)
        {
            if (!budget.spend(lookupWork))
            {
                result.workUsed = budget.used();
                return result;
            }
            if (identity.usedNodes.contains(static_cast<std::int32_t>(node)))
            {
                continue;
            }
            const auto column = futureNodeOfColumn(columns, identity.nodeColumns[node].slot);
            if (column != NO_SEQUENTIAL_NODE)
            {
                result.nodePrices[node] = hint[static_cast<std::size_t>(column)];
            }
        }
        result.workUsed = budget.used();
        result.completed = true;
        return result;
    }
}
